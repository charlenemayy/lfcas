/* Lock Free Contention Adapting Search Trees
 * COP4520 Spring 2019 (Dechev)
 * Charlene Juvida & Steve Dang
 *
 * This program was developed with POSIX threads, so it will only be compatible on Unix systems. Please test on UCF Eustis or another Linux/MacOS environment.
 * Instructions for connecting to Eustis can be found [here](http://www.cs.ucf.edu/~wocjan/Teaching/2016_Fall/cop3402/2_homeworks/eustis_tutorial.pdf)
 *
 * Based off the work done by K. Winblad, K. Sagonas and B. Jonsson in their paper of the
 * same name. The psuedocode that this script was based on can also be found in their paper
 * provided in the link below.
 *
 * http://user.it.uu.se/~bengt/Papers/Full/spaa18.pdf
 * https://github.com/kjellwinblad/JavaRQBench
 */
#include "lfcas.h"

template <class T>
class lfcatree {
	//=== Help Functions ================================
	private:
    // Insertion and Removal
    // Does a CAS to attempt to change the pointer of the base node's parent to
    // a new base node with the updated leaf container.
    bool try_replace(lfcat<T>* m, node<T>* b, node<T>* new_b) {
	    if(b->parent == NULL) {
	        return m->root.compare_exchange_weak(b, new_b, // cas
                   std::memory_order_release, std::memory_order_relaxed);
        } else if((&b->parent->left)->load() == b) { // b is on left
            /*
            lock.lock();
            std::cout << "checkpoint1" << "\n";
            std::cout << b->parent->left.compare_exchange_weak(b, new_b, // cas
                   std::memory_order_release, std::memory_order_relaxed) << "\n";
            lock.unlock();
            */
	        return b->parent->left.compare_exchange_weak(b, new_b, // cas
                   std::memory_order_release, std::memory_order_relaxed);
        } else if((&b->parent->right)->load() == b) { // b is on right
	        return b->parent->right.compare_exchange_weak(b, new_b, // cas
                   std::memory_order_release, std::memory_order_relaxed);
        } else {
            return false;
        }
	}

    // Insertion and Removal || Range Query
    // True if not in the first part of a join or not in a range query (where
    // its result is not set).
	bool is_replaceable(node<T>* n) {
        if(n == NULL) {
            return false;
        }
	    bool status = (n->type == normal ||
	    (n->type == joinmain &&
	      (&n->neigh2)->load() == aborted_status) ||
	    (n->type == joinneighbor &&
	     ((&n->main_node->neigh2)->load() == aborted_status ||
	      (&n->main_node->neigh2)->load() == done_status)) ||
	    (n->type == range &&
	     (&n->storage->result)->load() != not_set_status)); // current result is set
        return status;
	}

    // Insertion and Removal
    // Help other thread complete their function and guarantee progress.
    void help_if_needed(lfcat<T>* t, node<T>* n) {
        if(n == NULL || t == NULL) return;
        if(n->type == joinneighbor) n = n->main_node; // Node is in the middle of a join
        if(n->type == joinmain && (&n->neigh2)->load() == preparing_status) { // The neighbor of n has been joined
            (&n->neigh2)->compare_exchange_weak(preparing_status, aborted_status, // cas todo
            std::memory_order_release, std::memory_order_relaxed);

        } else if(n->type == joinmain && (&n->neigh2)->load() > aborted_status) { // Help the second phase of the join
        	complete_join(t, n);
        } else if(n->type == range && (&n->storage->result)->load() == not_set_status) { // Help the range query
        	all_in_range(t, n->lo, n->hi, n->storage);
        }
    }

    // Insertion and Removal || Range Query
    // Calculates the statistics value based on its base node and detected
    // contention. Make more fine-grained in high contention and vice versa.
    int new_stat(node<T>* n, contention_info info) {
    	int range_sub = 0;
    	if(n->type == range && (&n->storage->more_than_one_base)->load())
    		range_sub = RANGE_CONTRIB;
    	if (info == contended && n->stat <= HIGH_CONT) {
    		return n->stat + CONT_CONTRIB - range_sub;
    	} else if(info == uncontened && n->stat >= LOW_CONT) {
    		return n->stat - LOW_CONT_CONTRIB - range_sub;
    	} else return n->stat;
    }

    // Insertion and Removal || Range Query
    // Begin the process of adaptation
    void adapt_if_needed(lfcat<T>* t, node<T>* b) {
    	if(!is_replaceable(b)) {
            return;
        } else if(new_stat(b, noinfo) > HIGH_CONT) {
    		high_contention_adaptation(t, b);
        } else if(new_stat(b, noinfo) < LOW_CONT) {
    		low_contention_adaptation(t, b);
        }
    }

    // Insertion and Removal
    // Linearizable upon success; operation is retired upon failure. A
    // replacement attempt is made only if the found base node is replacable.
    // If it is not, it may be involved in another operation, and `do_update`
    // will first attempt to help this operation before proceeding.
    bool do_update(lfcat<T>* m, char mode, int i) {
    	contention_info cont_info = uncontened;
		node<T>* base;

    	while(true) {
    		base = find_base_node((&m->root)->load(), i);
    		if(is_replaceable(base)) {
 	   			bool res;
    			node<T>* newb;
                newb = new node<T>();

    			newb->type = normal;
				newb->parent = base->parent;

                if(mode == 'i')
				    newb->data = vector_insert(base->data, i, &res); // treap, int, boolean
                else if (mode == 'r')
				    newb->data = vector_remove(base->data, i, &res); // treap, int, boolean

				newb->stat = new_stat(base, cont_info);
    			if(try_replace(m, base, newb)) {
    				adapt_if_needed(m, newb);
    				return res;
    			}
			}
    	}
    	cont_info = contended;
    	help_if_needed(m, base);
    }

    //=== Vector Functions ==========================
    // Insertion and Removal
    std::vector<T>* vector_insert(std::vector<T>* node_data, int i, bool* res) {
        std::vector<T>* new_data = node_data;
        new_data->push_back(i);
        *res = true;
        return new_data;
    }

    // Insertion and Removal
    std::vector<T>* vector_remove(std::vector<T>* node_data, int i, bool* res) {
        std::vector<T>* new_data = node_data;
        new_data->erase(std::remove(new_data->begin(), new_data->end(), i));
        *res = true;
        return new_data;
    }

    // Lookup
    T vector_lookup(std::vector<T>* node_data, int i) {
        std::vector<T>* new_data = node_data;
        std::vector<int>::iterator it;
        it = std::find(node_data->begin(), node_data->end(), i);
        return *it;
    }

    // Range Query
    void vector_query(std::vector<T>* result) {
        /*
        lock.lock();
        std::cout << result << "\n";
        lock.unlock();
        */
    }

    // Range Query || Adaptations
    std::vector<T>* vector_join(std::vector<T>* a, std::vector<T>* b) {
        std::vector<T>* ab;
        ab->reserve(a->size() + b->size() ); // preallocate memory
        ab->insert( ab->end(), a->begin(), a->end() );
        ab->insert( ab->end(), b->begin(), b->end() );
        return ab;
    }

    // Adaptations
    // Get a suitable median value for the left and right base nodes' route node.
    int split_key(std::vector<T>* data) {
        return data->at(data->size() / 2);
    }

    // Adaptations
    // Split the data in half less than the route node key's value.
    std::vector<T>* split_left(std::vector<T>* data, int key) {
        std::vector<int>::iterator it;
        it = std::find(data->begin(), data->end(), key);
        int index = std::distance(data->begin(), it);

        std::vector<int>* left = new std::vector<int>(data->begin(), data->begin() + index + 1);
        return left;
    }

    // Adaptations
    // Split the data in half greater than the route node key's value.
    std::vector<T>* split_right(std::vector<T>* data, int key) {
        std::vector<int>::iterator it;
        it = std::find(data->begin(), data->end(), key);
        int index = std::distance(data->begin(), it);

        std::vector<int>* right = new std::vector<int>(data->begin() + index + 1, data->end());
        return right;
    }

    //=== Stack Functions ===========================

    // Range Query
    void push(stack<T>* s, node<T>* n) {
        if(s == NULL || s->stack_lib == NULL) {
            s->stack_lib = new std::stack<node<T>*>(); // stack_reset
            s->stack_array = new std::vector<node<T>*>();
        }
        s->stack_lib->push(n);
        s->stack_array->insert(s->stack_array->begin(), n);
        return;
    }

    // Range Query
    node<T>* pop(stack<T>* s) {
        if(s == NULL || s->stack_lib == NULL) return NULL;
        node<T>* n = s->stack_lib->top();

        s->stack_lib->pop();
        s->stack_array->erase(s->stack_array->begin());
        return n;
    }

    // Range Query
    node<T>* top(stack<T>* s) {
        return s->stack_lib->top();
    }

    // Range Query
    void replace_top(stack<T>* s, node<T>* n) {
        pop(s);
        push(s, n);
    }

    // Range Query
    stack<T> copy_state(stack<T>* s) {
        stack<T> q;
        q.stack_lib = s->stack_lib;
        q.stack_array = s->stack_array;
        return q;
    }

    //=== Public Interface ==========================
	public:
    std::mutex lock;
    node<T>* preparing_status;
    node<T>* done_status;
    node<T>* aborted_status;
    std::vector<T>* not_set_status;

    lfcatree() {
        preparing_status = (node<T>*)0;
        done_status = (node<T>*)1;
        aborted_status = (node<T>*)2;
        std::vector<T>* not_set_status = (std::vector<T>*)1;
    }

    // Insertion and Removal
    bool insert(lfcat<T>* m, int i) {
    	return do_update(m, 'i', i);
    }

    // Insertion and Removal
    bool remove(lfcat<T>* m, int i) {
    	return do_update(m, 'r', i);
    }

    // Lookup
    // Wait free. Traverses route nodes until base node is found, then performs
    // lookup in the corresponding immutable data structure.
    bool lookup(lfcat<T>* m, int i) {
    	node<T>* base = find_base_node((&m->root)->load(), i);
    	return vector_lookup(base->data, i);
    }

    // Range Query
    // Creates a snapshot of all base nodes in the requested range, then
    // traverses the snapshot to complete the range query
    void query(lfcat<T>* m, int lo, int hi) {
    	std::vector<T>* result = all_in_range(m, lo, hi, NULL);
    	vector_query(result);
    }

    // Lookup || Insertion and Removal
    // Finds base nodes but does not push the results to a stack like with
    // the range query functions below.
    node<T>* find_base_node(node<T>* n, int i) {
        if(n == NULL) return NULL;

        while(n->type == route) {
            if(i < n->key) {
                if(n->left == NULL) break;
                n = (&n->left)->load();
            } else {
                if(n->right == NULL) break;
                n = (&n->right)->load();
            }
        }
        return n;
    }

    // Range Query
    // Find base nodes in a depth first traversal through route nodes. Uses a
    // stack s to store the search path to the current base node.
    node<T>* find_base_stack(node<T>* n, int i, stack<T>* s) {
        if(s == NULL) {
            s = new stack<T>();
        }
        if(s->stack_lib == NULL || s->stack_array == NULL) {
            s->stack_lib = new std::stack<node<T>*>(); // stack_reset
            s->stack_array = new std::vector<node<T>*>();
        }

        if (n == NULL) return NULL;
        while(n->type == route) {
            push(s, n);
            if(i < n->key) {
                if(n->left == NULL) break;
                n = (&n->left)->load();
            } else {
                if(n->right == NULL) break;
                n = (&n->right)->load();
            }
        }
        push(s, n);
        return n;
    }

    // Range Query
    // Find base nodes in a depth first traversal. Uses a stack s to store the
    // search path to the current base node. Compared to the previous function,
    // the search begins from the current head of the stack
    node<T>* find_next_base_stack(stack<T>* s) {
        if(s == NULL) return NULL;
    	node<T>* base = pop(s);
    	node<T>* t = top(s);
    	if(t == NULL) return NULL;

    	if((&t->left)->load() == base)
    		return leftmost_and_stack((&t->right)->load(), s);

    	int be_greater_than = t->key;
    	while(t != NULL) {
    		if((&t->valid)->load() && t->key > be_greater_than)
    			return leftmost_and_stack((&t->right)->load(), s);
    		else {
				pop(s);
                t = top(s);
            }
    	}
    	return NULL;
    }

    // Range Query
    // Used for traversal.
    node<T>* leftmost_and_stack(node<T>* n, stack<T>* s) {
        while (n->type == route) {
            push(s, n);
            n = (&n->left)->load();
        }

        push(s, n);
        return n;
    }

    // Range Query
    // Initialize new range base.
    node<T>* new_range_base(node<T>* b, int lo, int hi, rs<T>* s) {
		node<T>* newrb;
		newrb = b;

		newrb->lo = lo;
		newrb->hi = hi;
		newrb->storage = s;
        newrb->type = range;
        return newrb;
	 }

    // Range Query
    // Goes through all base nodes that may contain items in range in ascending
    // key order. Replaces each base node by type `range_base` to indicate that it
    // is part of a range query.
    std::vector<T>* all_in_range(lfcat<T>* t, int lo, int hi, rs<T>* help_s) {
    	stack<T> s;
    	stack<T> backup_s;
    	node<T>* b;
    	rs<T>* my_s;

        find_first:b = find_base_stack((&t->root)->load(),lo,&s); // Find base nodes

    	if(help_s != NULL) { // result storage
                lock.lock();
                std::cout << "checkpoint2" <<  "\n";
                lock.unlock();
    		if(b->type != range || help_s != b->storage) { // update result query
    			return (&help_s->result)->load();
    		} else { // is a range base and storage has been set by another thread
			    my_s = help_s;
			}
    	} else if(is_replaceable(b)) { // result field != not_set_status
    		my_s = new rs<T>;
            my_s->result = new std::vector<T>();
            my_s->result.store(not_set_status);
            my_s->more_than_one_base = T();
            my_s->more_than_one_base.store(false);
    		node<T>* n = new_range_base(b, lo, hi, my_s); // new range base with updated result storage

    		if(!try_replace(t, b, n)) {
                goto find_first; // reset range query
            }
    		replace_top(&s, n);
    	} else if(b->type == range && b->hi >= hi) { // expand range query
    		return all_in_range(t, b->lo, b->hi, b->storage);
    	} else {
    		help_if_needed(t, b);
    		goto find_first;
    	}

    	stack<T> done;
        done = stack<T>();
        done.stack_lib = new std::stack<node<T>*>(); // stack_reset
        done.stack_array = new std::vector<node<T>*>();
    	while(true) { // Find remaining base nodes
	    	push(&done, b); // ultimate final result stack (NOT the route nodes)
            // NOTE: DONE IS NULL
	    	backup_s = copy_state(&s);

            std::vector<int>::iterator it; // get maximum value
            it = max_element(b->data->begin(), b->data->end());

	    	if (!b->data->empty() && *it >= hi) {
                /*
                lock.lock();
                std::cout << "\n";
                for(it = b->data->begin(); it != b->data->end(); ++it) {
                    std::cout << *it << " ";
                }
                std::cout << "\n";
                lock.unlock();
                */
				break;
            }
	    	find_next_base_node: b = find_next_base_stack(&s);
	    	if(b == NULL) {
                break; // out of base nodes
            }
	    	else if ((&my_s->result)->load() != not_set_status) { // range query is finished
	    		return (&my_s->result)->load();
	    	} else if (b->type == range && b->storage == my_s) { // b's storage is the same as the current
	    		continue;
	    	} else if (is_replaceable(b)) {
	    		node<T>* n = new_range_base(b, lo, hi, my_s); // change the type of node b is
	    		if(try_replace(t, b, n)) {
	    			replace_top(&s, n);
                    continue;
	    		} else {
	    			s = copy_state(&backup_s); // reset the stack
	    			goto find_next_base_node;
	    		}
	    	} else { // another thread has intercepted; help it out
	    		help_if_needed(t, b);
	    		s = copy_state(&backup_s); // reset stack
	    		goto find_next_base_node;
	    	}
    	}

    	std::vector<T>* res = done.stack_array->at(0)->data; // stack array is just an array of nodes
    	for(int i = 1; i < done.stack_array->size(); i++)
    		res = vector_join(res, done.stack_array->at(i)->data); // join all the data in the base nodes together

        if((&my_s->result)->compare_exchange_weak(not_set_status, res, // if still not set by another thread, replace
        std::memory_order_release, std::memory_order_relaxed)) { // && done.stack_lib->size() > 1) {
    	    (&my_s->more_than_one_base)->store(true);
        }

    	adapt_if_needed(t, done.stack_array->at(rand() % done.stack_array->size()));
    	return (&my_s->result)->load();
    }

    // Adaptations
    node<T>* deep_copy(node<T>* b) {
        node<T>* a = new node<T>();
        a->data = b->data;
        a->stat = b->stat;
        a->parent = b->parent;
        a->lo = b->lo; a->hi = b->hi;
        a->storage = b->storage;
        a->neigh1 = b->neigh1;
        a->gparent = b->gparent;
        a->otherb = b->otherb;
        a->main_node = b->main_node;

        node<T>* neigh2 = b->neigh2.load(); // cant copy atomics
        (&a->neigh2)->store(neigh2);

        return a;
    }

    // Adaptations
    node<T>* leftmost(node<T>* n) {
        while (n->type == route) {
            n = (&n->left)->load();
        }
        return n;
    }

    // Adaptations
    node<T>* rightmost(node<T>* n) {
        while (n->type == route) {
            n = (&n->right)->load();
        }
        return n;
    }

    // Adaptations
    node<T>* parent_of(lfcat<T>* t, node<T>* n) {
        node<T>* prev_node = NULL;
        node<T>* curr_node = (&t->root)->load();

        while(curr_node != n && curr_node->type == route) {
            prev_node = curr_node;
            if(n->key < curr_node->key) {
                curr_node = (&curr_node->left)->load();
            } else {
                curr_node = (&curr_node->right)->load();
            }
        }

        if(curr_node->type != route)
            return NOT_FOUND;

        return prev_node;
    }

    // Adaptations
    // The first phase of the join as described by the paper. Other threads
    // cannot help with this process. The corresponding diagrams are marked
    // on their place in the code.
    node<T>* secure_join_left(lfcat<T>* t, node<T>* b) {
        node<T>* n0 = leftmost((&b->parent->right)->load()); // get the neighboring node on the right
        if(!is_replaceable(n0)) return NULL;

        node<T>* m = deep_copy(b); // m is the main node
        m->type = joinmain; // mark that it is part of a join

        node<T>* nullvalue = nullptr;

        if(!(b->parent->left.compare_exchange_weak(b, m, // check that it's still on the left side and replace it (b)
        std::memory_order_release, std::memory_order_relaxed))) // cas
            return NULL;

        node<T>* n1 = deep_copy(n0);
        n1->main_node = m; // copy the neighboring node to replace it and change its type to join (c)

        if(!try_replace(t, n0, n1)) { // replace the neighboring node
    		(&m->neigh2)->store(aborted_status);
            return NULL;
        } else if(!(m->parent->join_id.compare_exchange_weak(nullvalue, m, // cas
        std::memory_order_release, std::memory_order_relaxed))) { // check that another thread has not attatched
                                                                // a join id and if not, set it (d)
    		(&m->neigh2)->store(aborted_status);
            return NULL;
        }

        node<T>* gparent = parent_of(t, m->parent); // set the join ids of the parents and grandparents to
                                                    // indicate that it is part of a join
        if(gparent == NOT_FOUND ||
          (gparent != NULL &&
           !(gparent->join_id.compare_exchange_weak(nullvalue, m, // cas
           std::memory_order_release, std::memory_order_relaxed)))) {
    		(&m->parent->join_id)->store(NULL);
            return NULL;
        }

        m->gparent = gparent; // set the information used for complete_join
        m->otherb = (&m->parent->right)->load(); // set to the actual value of right neighbor
        m->neigh1 = n1; // set to the expected value of the right neighbor

        node<T>* joinedp = m->otherb==n1 ? gparent: n1->parent; // set the main node's neigh2 field to n2, which
                                                                // will eventually replace both m and n1 in the
                                                                // complete_join (e)
        node<T>* n2 = deep_copy(n1);
        n2->parent = joinedp;
        n2->main_node = m;
        n2->data = vector_join(m->data, n1->data);

        if(m->neigh2.compare_exchange_weak(preparing_status, n2,
          std::memory_order_release, std::memory_order_relaxed)) return m; // should end here if CAS is successful

        if(gparent == NULL) {
    		(&m->parent->join_id)->store(NULL);
            return NULL;
        }
    	(&gparent->join_id)->store(NULL);

        return NULL;
    }

    // Adaptations
    // The first phase of the join as described by the paper. Other threads
    // cannot help with this process. The corresponding diagrams are marked
    // on their place in the code.
    node<T>* secure_join_right(lfcat<T>* t, node<T>* b) {
        node<T>* n0 = rightmost((&b->parent->left)->load()); // get the neighboring node on the left
        if(!is_replaceable(n0)) return NULL;

        node<T>* m = deep_copy(b); // m is the main node
        m->type = joinmain; // mark that it is part of a join

        node<T>* nullvalue = nullptr;

        if(!(b->parent->right.compare_exchange_weak(b, m,
          std::memory_order_release, std::memory_order_relaxed))) return NULL;

        node<T>* n1 = deep_copy(n0);
        n1->main_node = m; // copy the neighboring node to replace it and change its type to join (c)

        if(!try_replace(t, n0, n1)) { // replace the neighboring node
    		(&m->neigh2)->store(aborted_status);
            return NULL;
        } else if(!(m->parent->join_id.compare_exchange_weak(nullvalue, m,
                 std::memory_order_release, std::memory_order_relaxed))) { // check that another thread has not attatched
                                                                          // a join id and if not, set it (d)
    		(&m->neigh2)->store(aborted_status);
            return NULL;
        }

        node<T>* gparent = parent_of(t, m->parent); // set the join ids of the parents and grandparents to
                                                    // indicate that it is part of a join
        if(gparent == NOT_FOUND ||
          (gparent != NULL &&
           !(gparent->join_id.compare_exchange_weak(nullvalue, m,
           std::memory_order_release, std::memory_order_relaxed)))) {
    		(&m->parent->join_id)->store(NULL);
            return NULL;
        }

        m->gparent = gparent; // set the information used for complete_join
        m->otherb = (&m->parent->left)->load(); // set to the actual value of left neighbor
        m->neigh1 = n1; // set to the expected value of the left neighbor

        node<T>* joinedp = m->otherb==n1 ? gparent: n1->parent; // set the main node's neigh2 field to n2, which
                                                                // will eventually replace both m and n1 in the
                                                                // complete_join (e)
        node<T>* n2 = deep_copy(n1);
        n2->parent = joinedp;
        n2->main_node = m;
        n2->data = vector_join(m->data, n1->data);

        if(m->neigh2.compare_exchange_weak(preparing_status, n2, // should end here if CAS is successful
            std::memory_order_release, std::memory_order_relaxed)) return m;

        if(gparent == NULL) {
    		(&m->parent->join_id)->store(NULL);
            return NULL;
        }
        (&gparent->join_id)->store(NULL);

        return NULL;
    }

    // Adaptation
    // The second part of the join. Multiple threads can help out this
    // part of the join.
    void complete_join(lfcat<T>* t, node<T>* m) {
        node<T>* n2 = (&m->neigh2)->load();

        if(n2 == done_status) return;

        try_replace(t, m->neigh1, n2); // replace the neighbor node (f)
    	(&m->parent->valid)->store(false); // mark the main node's parent as false to prevent other threads from
                                          // traversing to it
        node<T>* replacement = (m->otherb == m->neigh1) ? n2 : m->otherb; // check that the neighbor node that was
                                                                          // replaced wasn't changed by another thread
        if (m->gparent == NULL) { // parent is spliced out in the following condition statements (g)
            (&t->root)->compare_exchange_weak(m->parent, replacement, // replacement is the node with the merged data
             std::memory_order_release, std::memory_order_relaxed);
        } else if((&m->gparent->left)->load() == m->parent) {
            (&m->gparent->left)->compare_exchange_weak(m->parent, replacement,
             std::memory_order_release, std::memory_order_relaxed);

            (&m->gparent->join_id)->compare_exchange_weak(m, NULL,
             std::memory_order_release, std::memory_order_relaxed);
        } else if((&m->gparent->right)->load() == m->parent) {
            (&m->gparent->right)->compare_exchange_weak(m->parent, replacement,
             std::memory_order_release, std::memory_order_relaxed);

            (&m->gparent->join_id)->compare_exchange_weak(m, NULL,
             std::memory_order_release, std::memory_order_relaxed);
        }
    	(&m->neigh2)->store(done_status); // n2 is now marked as replacable and the join has been completed (h)
    }

    // Adaptations
    // Join the contents of two base nodes into one base node
    void low_contention_adaptation(lfcat<T>* t, node<T>* b) {
        if(b->parent == NULL) return;
        if((&b->parent->left)->load() == b) { // check what side the node is on
            node<T>* m = secure_join_left(t, b);
            if (m != NULL) complete_join(t, m);
        } else if ((&b->parent->right)->load() == b) { // check what side the node is on
            node<T>* m = secure_join_right(t, b);
            if (m != NULL) complete_join(t, m);
        }
    }

    // Adaptations
    // Split the contents of one base node into two base nodes
    void high_contention_adaptation(lfcat<T>* m, node<T>* b) {
        if(b->data->size() < 2) return;

        std::vector<T>* data = b->data;
        std::sort(data->begin(), data->end());

        node<T>* r = new node<T>(); // create new route node to hold two new base nodes
        r->type = route;
        r->key = split_key(data);
        r->valid = true;

        node<T>* left = new node<T>();
        left->type = normal;
        left->parent = r;
        left->stat = 0;
        left->data = split_left(data, r->key);
        r->left = left;

        node<T>* right = new node<T>();
        right->type = normal;
        right->parent = r;
        right->stat = 0;
        right->data = split_right(data, r->key);
        r->right = right;

        try_replace(m, b, r);
    }

    //=== Test Functions ================================
    node<T>* new_route_node(T key) {
        node<T>* r = new node<T>();
        r->key = key;
        r->type = route;
        return r;
    }

    node<T>* new_base_node(std::vector<T>* data) {
        node<T>* b = new node<T>();
        b->data = data;
        b->type = normal;
        return b;
    }

    void test() {
        node<T>* r0 = new_route_node(70); // fill the lfca tree with data
        node<T>* r1 = new_route_node(40);
        node<T>* r2 = new_route_node(80);
        node<T>* r3 = new_route_node(60);

        std::vector<int>* r1_ldata = new std::vector<int>{35, 36, 37};
        std::vector<int>* r2_ldata = new std::vector<int>{75, 76, 77};
        std::vector<int>* r2_rdata = new std::vector<int>{85, 86, 87};
        std::vector<int>* r3_ldata = new std::vector<int>{55, 56, 57};
        std::vector<int>* r3_rdata = new std::vector<int>{65, 66, 67};

        node<T>* r1_lbase = new_base_node(r1_ldata);
        node<T>* r2_lbase = new_base_node(r2_ldata);
        node<T>* r2_rbase = new_base_node(r2_rdata);
        node<T>* r3_lbase = new_base_node(r3_ldata);
        node<T>* r3_rbase = new_base_node(r3_rdata);

        r0->left = r1;
        r0->right = r2;

        r1->parent = r0;
        r1->left = r1_lbase;
        r1->right = r3;

        r2->parent = r0;
        r2->left = r2_lbase;
        r2->right = r2_rbase;

        r3->parent = r1;
        r3->left = r3_lbase;
        r3->right = r3_rbase;

        r1_lbase->parent = r1;
        r2_lbase->parent = r2;
        r2_rbase->parent = r2;
        r3_lbase->parent = r3;
        r3_rbase->parent = r3;

        lfcat<T>* tree = new lfcat<T>();
        tree->root = r0;

        pthread_t threads[NUM_THREADS];
        struct arg_struct<T> args[NUM_THREADS];

        // Insert
        for (int i = 0; i < NUM_THREADS; i++) {
            args[i].tid = i;
            args[i].tree = tree;
            args[i].self = this;
            pthread_create(&threads[i], NULL, insert_test, (void *)&args[i]);
        }
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        // Lookups
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_create(&threads[i], NULL, lookup_test, (void *)&args[i]);
        }
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        // Range Queries
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_create(&threads[i], NULL, query_test, (void *)&args[i]);
        }
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    static void *insert_test(void* args) {
        struct arg_struct<T> *info = (struct arg_struct<T>*)args;
        int tid = info->tid;
        lfcat<T>* tree = info->tree;
        void *self = info->self;

        printf("starting insertion for thread %d\n", tid);

        for(int i = 0; i < NUM_UPDATE; i++) {
            static_cast <lfcatree<T>*>(self)->insert(tree, (tid * 10) + i);
        }
        pthread_exit(NULL);
    }

    static void *lookup_test(void* args) {
        struct arg_struct<T> *info = (struct arg_struct<T>*)args;
        int tid = info->tid;
        lfcat<T>* tree = info->tree;
        void *self = info->self;

        printf("starting lookup for thread %d\n", tid);

        for(int i = 0; i < NUM_LOOKUP; i++) {
            static_cast <lfcatree<T>*>(self)->lookup(tree, (tid * 10) + i);
        }
        pthread_exit(NULL);
    }

    static void *query_test(void* args) {
        struct arg_struct<T> *info = (struct arg_struct<T>*)args;
        int tid = info->tid;
        lfcat<T>* tree = info->tree;
        void *self = info->self;

        printf("starting query for thread %d\n", tid);

        for(int i = 0; i < NUM_QUERY; i++) {
            static_cast <lfcatree<T>*>(self)->query(tree, (tid * 10), (tid * 10) + 9);
        }
        pthread_exit(NULL);
    }

   static void *remove_test(void* args) {
        struct arg_struct<T> *info = (struct arg_struct<T>*)args;
        int tid = info->tid;
        lfcat<T>* tree = info->tree;
        void *self = info->self;

        printf("starting removal for thread %d\n", tid);

        for(int i = 0; i < NUM_UPDATE; i++) {
            static_cast <lfcatree<T>*>(self)->remove(tree, (tid * 10) + i);
        }
        pthread_exit(NULL);
    }
};

int main () {
    lfcatree<int> lfca;
    lfca.test();
    return 0;
}

