#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <time.h>
#include <unistd.h>
#include <fstream>
#include <stack>
#include <atomic>
#include <vector>
#include <chrono>
#include <set>

//=== Constants =====================================
#define CONT_CONTRIB 250 // For adaptation
#define LOW_CONT_CONTRIB 1 // ...
#define RANGE_CONTRIB 100 // ...
#define HIGH_CONT 1000 // ...
#define LOW_CONT -1000 // ...
#define NOT_FOUND (node<T>*)1 // Special pointers
#define NUM_THREADS 10
#define NUM_UPDATE 40
#define NUM_LOOKUP 40
#define NUM_QUERY 20
enum contention_info { contended , uncontened , noinfo };
enum node_type {
    route, normal, joinmain, joinneighbor, range
};
//=== Data Structures ===============================
template <class T>
struct rs { // Result storage for range queries (list of values)
    rs() : more_than_one_base(false) {}
    std::atomic<std::vector<T>*> result; // The result
    std::atomic<T> more_than_one_base;
};
template <class T>
struct node {
    node() : join_id(NULL), valid(true), neigh2((node<T>*)0) {}
    node_type type;

    // normal_base
    std::vector<T>* data = NULL; // Items in the set
    int stat = 0; // Statistics variable
    node<T>* parent = NULL; // Parent node or NULL (root)

    // range_base
    int lo; int hi; // Low and high key
    rs<T>* storage;

    // join_main
    node<T>* neigh1; // First (not joined) neighbor base
    std::atomic<node<T>*> neigh2; // Joined n...
    node<T>* gparent; // Grand parent
    node<T>* otherb; // Other branch

    // join_neighbor
    node<T>* main_node; // The main node for the join

	// route_node
    int key; // Split key
    std::atomic<node<T>*> left; // < key
    std::atomic<node<T>*> right; // >= key
    std::atomic<T> valid; // Used for join
    std::atomic<node<T>*> join_id; // ...
};
template <class T>
struct lfcat{
    std::atomic<node<T>*> root;
};
template <class T>
struct stack { // for storing base nodes
    std::stack<node<T>*>* stack_lib;
	std::vector<node<T>*>* stack_array;
};
//=== Test Structures ===============================
template <class T>
struct arg_struct {
    lfcat<T>* tree;
    int tid;
    void* self;
};
