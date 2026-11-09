#include "Common.h"
#include "GameSystem/TypeContainer.h"
#include <cassert>
struct A {}; struct B {}; struct C {};
int main() {
 TypeUnorderedMapContainer<TYPELIST_3(A,B,C),unsigned> store;
 assert(store.snapshot<A>().empty());
 A a; B b; C c; store.insert(1u,&a);store.insert(2u,&b);store.insert(3u,&c);
 auto snapshot=store.snapshot<B>();
 assert(snapshot.size()==1 && snapshot.front()==&b);
 assert(store.snapshot<A>().front()==&a && store.snapshot<C>().front()==&c);
 store.erase(2u,&b);
 assert(store.snapshot<B>().empty());
 assert(snapshot.size()==1 && snapshot.front()==&b);
 assert(store.find(1u,(A*)nullptr)==&a && store.find(3u,(C*)nullptr)==&c);
}
