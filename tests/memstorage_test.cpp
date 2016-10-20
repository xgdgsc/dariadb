#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <libdariadb/ads/fixed_tree.h>
#include <libdariadb/ads/lockfree_array.h>
#include <libdariadb/storage/memstorage.h>
#include <libdariadb/meas.h>

#include <boost/date_time/posix_time/posix_time.hpp>

BOOST_AUTO_TEST_CASE(LockFreeArrayTypeTraitTest) {
  dariadb::ads::LockFreeArray<int> lf(10);
  BOOST_CHECK_EQUAL(lf.size(), size_t(10));

  dariadb::ads::LockFreeArray<int *> lfs(10);
  BOOST_CHECK_EQUAL(lfs.size(), size_t(10));
}

BOOST_AUTO_TEST_CASE(ArrayLockFreeTest) {
  {
    dariadb::ads::LockFreeArray<bool> lf(3);
    BOOST_CHECK_EQUAL(lf.size(), size_t(3));

    lf.store(0, true);
    lf.store(1, true);

    dariadb::ads::LockFreeArray<bool> lf_c(std::move(lf));
    BOOST_CHECK(lf_c[0]);
    BOOST_CHECK(lf_c[1]);
    BOOST_CHECK(!lf_c[2]);

    bool expected = false;
    BOOST_CHECK(!lf_c.compare_exchange(0, expected, false));
    BOOST_CHECK(expected);
    expected = true;
    BOOST_CHECK(lf_c.compare_exchange(0, expected, false));
    BOOST_CHECK(!lf_c[0]);
  }
  {
    dariadb::ads::LockFreeArray<bool> lf(5);
    BOOST_CHECK_EQUAL(lf.cap(), lf.size());

    size_t i = 0;
    while (lf.insert(true)) {
      i++;
    }
    // check ctors
    dariadb::ads::LockFreeArray<bool> midle(lf);
    dariadb::ads::LockFreeArray<bool> lf_c = midle;

    BOOST_CHECK_EQUAL(lf_c.size(), i);
    BOOST_CHECK_EQUAL(lf_c.cap(), size_t(0));
    for (i = 0; i < lf_c.size(); ++i) {
      BOOST_CHECK(lf_c[i]);
    }
  }
}

template <class T> struct KeySplitter {
  static const size_t levels_count = sizeof(T);
  typedef std::array<size_t, levels_count> splited_key;
  size_t level_size(size_t level_num) const {
    auto res = std::pow(2, sizeof(uint8_t) * 8);
    return res;
  }

  splited_key split(const T &k) const {
    splited_key result;
    auto in_bts = reinterpret_cast<const uint8_t *>(&k);
    for (size_t i = 0; i < levels_count; ++i) {
      result[levels_count - i - 1] = in_bts[i];
    }
    return result;
  }
};

template <typename T> struct Statistic {
  void append(const T &t) {}
};

BOOST_AUTO_TEST_CASE(FixedTreeTypeTraitsTest) {
  dariadb::ads::FixedTree<dariadb::Time, dariadb::Meas, KeySplitter<dariadb::Time>,
                          Statistic<dariadb::Meas>>
      tree;
  BOOST_CHECK_EQUAL(tree.keys_count(), size_t(0));
}

BOOST_AUTO_TEST_CASE(FixedTreeNodeTest) {
  using MeasTree =
      dariadb::ads::FixedTree<dariadb::Time, dariadb::Meas, KeySplitter<dariadb::Time>,
                              Statistic<dariadb::Meas>>;
  MeasTree tree;
  MeasTree::Node node2(&tree, 0, 2);
  BOOST_CHECK(!node2.childExists(0));
  BOOST_CHECK(!node2.childExists(1));

  auto child0 = node2.create_or_get(0);
  auto child1 = node2.create_or_get(1);

  BOOST_CHECK(child0 != nullptr);
  BOOST_CHECK(child1 != nullptr);

  auto child01 = node2.create_or_get(0);
  auto child11 = node2.create_or_get(1);

  BOOST_CHECK_EQUAL(child0, child01);
  BOOST_CHECK_EQUAL(child1, child11);
}

BOOST_AUTO_TEST_CASE(FixedTreeNodeInsertionTest) {
  using TestTree =
      dariadb::ads::FixedTree<uint16_t, int, KeySplitter<uint16_t>, Statistic<int>>;
  TestTree tree;
  uint16_t K1 = 0;
  int V1 = 1;
  tree.insert(K1, V1);
  BOOST_CHECK_EQUAL(tree.keys_count(), size_t(1));
  int result = 0;
  BOOST_CHECK(tree.find(K1, &result));
  BOOST_CHECK_EQUAL(result, V1);

  for (uint16_t i = 1; i < 1000; ++i) {
    auto cur_V = int(i);
    tree.insert(i, int(i));
    BOOST_CHECK(tree.find(i, &result));
    BOOST_CHECK_EQUAL(result, cur_V);
  }

  for (uint16_t i = 2000; i > 1500; --i) {
    auto cur_V = int(i);
    tree.insert(i, int(i));
    BOOST_CHECK(tree.find(i, &result));
    BOOST_CHECK_EQUAL(result, cur_V);
  }

  for (uint16_t i = 1100; i < 1300; ++i) {
    auto cur_V = int(i);
    tree.insert(i, int(i));
    BOOST_CHECK(tree.find(i, &result));
    BOOST_CHECK_EQUAL(result, cur_V);
  }
}


BOOST_AUTO_TEST_CASE(MemStorage) {
    using boost::posix_time::seconds;
  const dariadb::Time Step3s = seconds(2).total_milliseconds();
  const dariadb::Time Step2s = seconds(2).total_milliseconds();
  const dariadb::Time Step1s = seconds(1).total_milliseconds();

  const size_t VALUES_COUNT = 100;

  dariadb::storage::MemStorage::Params p;
  dariadb::storage::MemStorage ms{p};

  dariadb::MeasArray ma(VALUES_COUNT);
  for (size_t i = 0; i < VALUES_COUNT; ++i) {
    ma[i].id = 0;
    ma[i].time = i * Step1s;
  }
  ms.append(ma.begin(), ma.end());


  for (size_t i = 0; i < VALUES_COUNT; ++i) {
    ma[i].id = 0;
    ma[i].time = i * Step2s;
  }
  ms.append(ma.begin(), ma.end());


  for (size_t i = 0; i < VALUES_COUNT; ++i) {
    ma[i].id = 0;
    ma[i].time = i * Step3s;
  }
  ms.append(ma.begin(), ma.end());
//  BOOST_CHECK_EQUAL(ms.append(Step1s, ma).ignored, size_t(0));
//  BOOST_CHECK_EQUAL(ms.append(Step2s, ma).ignored, size_t(0));
//  BOOST_CHECK_EQUAL(ms.append(Step3s, ma).ignored, size_t(0));
}
