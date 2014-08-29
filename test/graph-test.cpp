#include "gtest/gtest.h"

#include <vector>

#include "graph.h"

using namespace std;
using namespace simit;

//// Set tests

TEST(SetTests, Utils) {
  ASSERT_EQ(typeOf<int>(), Type::INT);
  ASSERT_EQ(typeOf<double>(), Type::FLOAT);
}

TEST(SetTests, AddAndGetFromTwoFields) {
  Set myset;
  
  FieldHandle f1 = myset.addField(Type::INT, "intfld");
  FieldHandle f2 = myset.addField(Type::FLOAT, "floatfld");
  
  ASSERT_EQ(myset.getSize(), 0);
  
  ElementHandle i = myset.addElement();
  myset.set(i, f1, 10);
  myset.set(i, f2, 101.1);
  
  ASSERT_EQ(myset.getSize(), 1);
  
  double ret;
  int ret2;
  myset.get(i, f1, &ret2);
  ASSERT_EQ(ret2, 10);
  
  
  myset.get(i, f2, &ret);
  ASSERT_EQ(ret, 101.1);
  
}

TEST(SetTests, IncreaseCapacity) {
  Set myset;
  
  auto fld = myset.addField(Type::INT, "foo");
  
  for (int i=0; i<1029; i++) {
    auto item = myset.addElement();
    myset.set(item, fld, i);
  }

  int count = 0;
  bool foundIt[1029];
  for (auto b : foundIt)
    b = false;
  
  for (auto it : myset) {
    int val;
    myset.get(it, fld, &val);
    foundIt[val] = true;
    count++;
  }
  
  for (int i=0; i<1029; i++)
    ASSERT_TRUE(foundIt[i]);
  ASSERT_EQ(count, 1029);
}

TEST(SetTests, FieldAccesByName) {
  Set myset;
  
  auto f1 = myset.addField(Type::FLOAT, "fltfld");
  auto f2 = myset.addField(Type::FLOAT, "fltfld2");
  
  ASSERT_EQ(myset.getField("fltfld"), f1);
  ASSERT_EQ(myset.getField("fltfld2"), f2);
}

//// Iterator tests
TEST(ElementIteratorTests, TestElementIteratorLoop) {
  Set myset;
  
  FieldHandle f1 = myset.addField(Type::INT, "intfld");
  FieldHandle f2 = myset.addField(Type::FLOAT, "floatfld");

  ASSERT_EQ(myset.getSize(), 0);
  
  for (int i=0; i<10; i++) {
    auto el = myset.addElement();
    myset.set(el, f1, 5+i);
    myset.set(el, f2, 10.0+(double)i);
  }
  
  int howmany=0;
  for (Set::ElementIterator it=myset.begin(); it<myset.end(); it++) {
    auto el = *it;
    int val;
    myset.get(el, f1, &val);
    ASSERT_TRUE((val>=5) && (val<15));
    howmany++;
  }
  ASSERT_EQ(howmany, 10);
  
  howmany=0;
  for (auto it : myset) {
    auto el = it;
    int val;
    myset.get(el, f1, &val);
    ASSERT_TRUE((val>=5) && (val<15));
    howmany++;
  }
  ASSERT_EQ(howmany, 10);
}