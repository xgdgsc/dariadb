#include "page_manager.h"
#include "../utils/utils.h"
#include "page.h"
#include "../utils/fs.h"
#include "../utils/locker.h"

#include <cstring>
#include <queue>
#include <thread>

using namespace dariadb::storage;
dariadb::storage::PageManager* PageManager::_instance = nullptr;

class PageManager::Private
{
public:
    Private(const PageManager::Params&param) :
        _cur_page(nullptr),
		_param(param)
    {
		_write_thread_stop = false;
		_write_thread_handle = std::move(std::thread{ &PageManager::Private::write_thread,this });
	}
	
	
    ~Private() {
		_write_thread_stop = true;
		_data_cond.notify_one();
		_write_thread_handle.join();

		if (_cur_page != nullptr) {
			delete _cur_page;
			_cur_page = nullptr;
		}
    }

    uint64_t calc_page_size()const {
        auto sz_index = sizeof(Page_ChunkIndex)*_param.chunk_per_storage;
        auto sz_buffers = _param.chunk_per_storage*_param.chunk_size;
        return sizeof(PageHeader)
                + sz_index
                + sz_buffers;
    }

    Page* create_page() {
        if (!dariadb::utils::fs::path_exists(_param.path)) {
            dariadb::utils::fs::mkdir(_param.path);
        }

        std::string page_name = ((_param.mode == MODE::SINGLE) ? "single.page" : "_.page");
        std::string file_name = dariadb::utils::fs::append_path(_param.path, page_name);

        Page*res = nullptr;

        if (!utils::fs::path_exists(file_name)) {
            auto sz = calc_page_size();
            res = Page::create(file_name, sz,_param.chunk_per_storage,_param.chunk_size,_param.mode);
        }
        else {
            res = Page::open(file_name);
        }
        return res;
    }
	void  flush() {
		const std::chrono::milliseconds sleep_time = std::chrono::milliseconds(100);
		while (!this->_in_queue.empty()) {
			std::this_thread::sleep_for(sleep_time);
		}
	}
	void write_thread() {
		while (!_write_thread_stop) {
			std::unique_lock<std::mutex> lk(_locker);
			_data_cond.wait(lk, [&] {return !_in_queue.empty() || _write_thread_stop; });
			while (!_in_queue.empty()) {
				auto ch = _in_queue.front();
				_in_queue.pop();
			
				write_to_page(ch);
			}
		}
	}


    Page* get_cur_page() {
        if (_cur_page == nullptr) {
            _cur_page = create_page();
        }
        return _cur_page;
    }

	bool write_to_page(const Chunk_Ptr&ch) {
		std::lock_guard<std::mutex> lg(_locker_write);
		auto pg = get_cur_page();
		return pg->append(ch);
	}

    bool append(const Chunk_Ptr&ch) {
        std::lock_guard<std::mutex> lg(_locker);
		_in_queue.push(ch);
		_data_cond.notify_one();
        return true;
    }

	bool append(const ChunksList&lst) {
        for(auto &c:lst){
            if(!append(c)){
                return false;
            }
        }
        return true;
    }

    Cursor_ptr chunksByIterval(const IdArray &ids, Flag flag, Time from, Time to){
        std::lock_guard<std::mutex> lg(_locker);
		auto p = get_cur_page();
		return p->chunksByIterval(ids, flag, from, to);
    }

    IdToChunkMap chunksBeforeTimePoint(const IdArray &ids, Flag flag, Time timePoint){
        std::lock_guard<std::mutex> lg(_locker);

		auto cur_page = this->get_cur_page();

		return cur_page->chunksBeforeTimePoint(ids, flag, timePoint);
    }

	
    dariadb::IdArray getIds() {
        std::lock_guard<std::mutex> lg(_locker);
        if(_cur_page==nullptr){
            return dariadb::IdArray{};
        }
		auto cur_page = this->get_cur_page();
		return cur_page->getIds();
    }

	dariadb::storage::ChunksList get_open_chunks() {
		if(!dariadb::utils::fs::path_exists(_param.path)) {
			return ChunksList{};
		}
		return this->get_cur_page()->get_open_chunks();
	}

	size_t chunks_in_cur_page() const {
		if (_cur_page == nullptr) {
			return 0;
		}
        return _cur_page->header->addeded_chunks;
	}
	size_t  in_queue_size()const {
		return _in_queue.size();
	}

    dariadb::Time minTime(){
        std::lock_guard<std::mutex> lg(_locker);
        if(_cur_page==nullptr){
            return dariadb::Time(0);
        }else{
            return _cur_page->header->minTime;
        }
    }

    dariadb::Time maxTime(){
        std::lock_guard<std::mutex> lg(_locker);
        if(_cur_page==nullptr){
            return dariadb::Time(0);
        }else{
            return _cur_page->header->maxTime;
        }
    }
protected:
    Page*  _cur_page;
	PageManager::Params _param;
    std::mutex _locker,_locker_write;

	std::queue<Chunk_Ptr> _in_queue;
	bool        _write_thread_stop;
	std::thread _write_thread_handle;
	std::condition_variable _data_cond;
};

PageManager::PageManager(const PageManager::Params&param):
    impl(new PageManager::Private{param})
{}

PageManager::~PageManager() {
}

void PageManager::start(const PageManager::Params&param){
    if(PageManager::_instance==nullptr){
        PageManager::_instance=new PageManager(param);
    }
}

void PageManager::stop(){
    if(_instance!=nullptr){
        delete PageManager::_instance;
        _instance = nullptr;
    }
}

void  PageManager::flush() {
	this->impl->flush();
}

PageManager* PageManager::instance(){
    return _instance;
}

bool PageManager::append(const Chunk_Ptr&c){
    return impl->append(c);
}

bool PageManager::append(const ChunksList&c){
    return impl->append(c);
}

//Cursor_ptr PageManager::get_chunks(const dariadb::IdArray&ids, dariadb::Time from, dariadb::Time to, dariadb::Flag flag) {
//    return impl->get_chunks(ids, from, to, flag);
//}

dariadb::storage::Cursor_ptr PageManager::chunksByIterval(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time from, dariadb::Time to){
    return impl->chunksByIterval(ids,flag,from,to);
}

dariadb::storage::IdToChunkMap PageManager::chunksBeforeTimePoint(const dariadb::IdArray &ids, dariadb::Flag flag, dariadb::Time timePoint){
    return impl->chunksBeforeTimePoint(ids,flag,timePoint);
}

dariadb::IdArray PageManager::getIds() {
    return impl->getIds();
}

dariadb::storage::ChunksList PageManager::get_open_chunks() {
	return impl->get_open_chunks();
}

size_t PageManager::chunks_in_cur_page() const{
	return impl->chunks_in_cur_page();
}

size_t PageManager::in_queue_size()const{
	return impl->in_queue_size();
}

dariadb::Time PageManager::minTime(){
    return impl->minTime();
}

dariadb::Time PageManager::maxTime(){
    return impl->maxTime();
}
