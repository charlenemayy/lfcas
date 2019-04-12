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
	treap<T>* lfcatreap;

    // All
    // Load atomic variable.
    T aload(T &t) {
        node<T>* a = *t.load();
        std::cout << &t << " " << *t << " " << a;
        return a;
    }

    // All
    // Store atomic variable.
    void astore(T &t, T &s) {
        *t.store(*s);
        return;
    }

    // All
    bool CAS(node<T>* expected, node<T>* actual, node<T>* replace) {
       return expected->compare_exchange_weak(actual, replace,
              std::memory_order_release, std::memory_order_relaxed);
    }

    // Insertion and Removal
    // Does a CAS to attempt to change the pointer of the base node's parent to
    // a new base node with the updated leaf container.
    bool try_replace(lfcat<T>* m, node<T>* b, node<T>* new_b) {
	    if( b->parent == NULL )
	        return CAS(&m->root, b, new_b);
	    else if(aload(&b->parent->left) == b)
	        return CAS(&b->parent->left, b, new_b);
	    else if(aload(&b->parent->right) == b)
	        return CAS(&b->parent->right, b, new_b);
	    else return false;
	}

    // Insertion and Removal || Range Query
    // True if not in the first part of a join or not in a range query (where
    // its result is not set).
	bool is_replaceable(node<T>* n) {
	    return (n->type == normal ||
	    (n->type == joinmain &&
	      aload(&n->neigh2) == ABORTED) ||
	    (n->type == joinneighbor &&
	     (aload(&n->main_node->neigh2) == ABORTED ||
	      aload(&n->main_node->neigh2) == DONE)) ||
	    (n->type == range &&
	     aload(&n->storage ->result) != NOT_SET));
	}

    // Insertion and Removal
    // Help other thread complete their function and guarantee progress.
    void help_if_needed(lfcatree* t, node<T>* n) {
        if(n->type == joinneighbor) n = n->main_node; // Node is in the middle of a join
        if(n->type == joinmain && aload(&n->neigh2) == PREPARING) { // The neighbor of n has been joined
        	CAS(&n->neigh2 , PREPARING , ABORTED);
        } else if(n->type == joinmain && aload(&n->neigh2) > ABORTED) { // Help the second phase of the join
        	complete_join(t, n);
        } else if(n->type == range && aload(&n->storage ->result) == NOT_SET) { // Help the range query
        	all_in_range(t, n->lo, n->hi, n->storage);
        }
    }

    // Insertion and Removal || Range Query
    // Calculates the statistics value based on its base node and detected
    // contention. Make more fine-grained in high contention and vice versa.
    int new_stat(node<T>* n, contention_info info) {
    	int range_sub = 0;
    	if(n->type == range && aload(&n->storage ->more_than_one_base))
    		range_sub = RANGE_CONTRIB;
    	if (info == contended && n->stat <= HIGH_CONT) {
    		return n->stat + CONT_CONTRIB - range_sub;
    	} else if(info == uncontened && n->stat >= LOW_CONT) {
    		return n->stat - LOW_CONT_CONTRIB - range_sub;
    	} else return n->stat;
    }

    // Insertion and Removal || Range Query
    // Begin the process of adaptation
    void adapt_if_needed(lfcatree* t, node<T>* b) {
    	if(!is_replaceable(b)) return;
    	else if(new_stat(b, noinfo) > HIGH_CONT)
    		high_contention_adaptation(t, b);
    	else if(new_stat(b, noinfo) < LOW_CONT)
    		low_contention_adaptation(t, b);
    }

    // Insertion and Removal
    // Linearizable upon success; operation is retired upon failure. A
    // replacement attempt is made only if the found base node is replacable.
    // If it is not, it may be involved in another operation, and `do_update`
    // will first attempt to help this operation before proceeding.
    bool do_update(lfcatree* m, treap<T>*(*u)(treap<T>*,int,bool*), int i) {
    	contention_info cont_info = uncontened;
		node<T>* base;
    	while(true) {
    		base = find_base_node(aload(&m->root), i);
    		if(is_replaceable(base)) {
 	   			bool res;
    			node<T>* newb;

    			newb->node_type = normal;
				newb->parent = base->parent;
				newb->data = u(base->data, i, &res);
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

    //=== Stack Functions ===========================
    // Range Query
    stack<T>* stack_reset(stack<T>* s) {
        s = new stack<T>*();
        // s->stack_array.clear();
        return s;
    }

    // Range Query
    void push(stack<T>* s, node<T>* n) {
        s->stack_lib->push(n);
        s->stack_array.insert(s->stack_array.begin(), n);
        return;
    }

    // Range Query
    node<T>* pop(stack<T>* s) {
        node<T>* n = s->stack_lib->pop();
        s->stack_array.erase(s->stack_array.begin());
        return n;
    }

    // Range Query
    node<T>* top(stack<T>* s) {
        return s->stack_lib->top();
    }

    // Range Query
    void replace_top(stack<T>* s, node<T>* n) {
        pop(s->stack);
        push(s->stack, n);
    }

    // Range Query
    stack<T>* copy_state(stack<T>* s) {
        stack<T>* q = new stack<T>*();
        q->stack_lib = s->stack_lib;
        q->stack_array = s->stack_array;
        return s;
    }

    //=== Treap Functions ===========================

    //=== Public Interface ==========================
	public:
	lfcatree() { // TO-DO: constructor
	}

    // Insertion and Removal
    bool insert(lfcat<T>* m, int i) {
    	return do_update(m, lfcatreap->insert, i);
    }

    // Insertion and Removal
    bool remove(lfcat<T>* m, int i) {
    	return do_update(m, lfcatreap->remove, i);
    }

    // Lookup
    // Wait free. Traverses route nodes until base node is found, then performs
    // lookup in the corresponding immutable data structure.
    bool lookup(lfcat<T>* m, int i) {
    	node<T>* base = find_base_node(aload(&m->root), i);
    	return lfcatreap->lookup(base->data, i);
    }

    // Range Query
    // Creates a snapshot of all base nodes in the requested range, then
    // traverses the snapshot to complete the range query
    void query(lfcat<T>* m, int lo, int hi, void (*trav)(int, void*), void* aux) {
    	treap<T>* result = all_in_range(m, lo, hi, NULL);
    	lfcatreap->query(result , lo, hi, trav, aux);
    }

    // Lookup || Insertion and Removal
    // Finds base nodes but does not push the results to a stack like with
    // the range query functions below.
    node<T>* find_base_node(node<T>* n, int i) {
        while(n->type == route) {
            if(i < n->key) {
                n = aload(&n->left);
            } else {
                n = aload(&n->right);
            }
        }
        return n;
    }

    // Range Query
    // Find base nodes in a depth first traversal through route nodes. Uses a
    // stack s to store the search path to the current base node.
    node<T>* find_base_stack(node<T>* n, int i, stack<T>* s) {
        s = stack_reset(s);
        while(n->type == route) {
            push(s, n);
            if (i < n->key) {
                n = aload(&n->left);
            } else {
                n = aload(&n->right);
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
    	node<T>* base = pop(s);
    	node<T>* t = top(s);
    	if(t == NULL) return NULL;

    	if(aload(&t->left) == base)
    		return leftmost_and_stack(aload(&t->right), s);

    	int be_greater_than = t->key;
    	while(t != NULL) {
    		if(aload(&t->valid) && t->key > be_greater_than)
    			return leftmost_and_stack(aload(&t->right), s);
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
            n = aload(&n->left);
        }

        push(s, n);
        return n;
    }

    // Range Query
    // Initialize new range base.
    node<T>* new_range_base(node<T>* b, int lo, int hi, rs<T>* s) {
		node<T>* newrb;
		newrb = b;

		b->lo = lo;
		b->hi = hi;
		b->storage = s;
	 }

    // Range Query
    // Goes through all base nodes that may contain items in range in ascending
    // key order. Replaces each base node by type `range_base` to indicate that it
    // is part of a range query.
    treap<T>* all_in_range(lfcat<T>* t, int lo, int hi, rs<T>* help_s) {
    	stack<T>* s;
    	stack<T>* backup_s;
    	stack<T>* done;
    	node<T>* b;
    	rs<T>* my_s;

        find_first:b = find_base_stack(aload(&t->root),lo,s); // Find base nodes

    	if(help_s != NULL) { // result storage
    		if(b->type != range || help_s != b->storage) { // update result query
    			return aload(&help_s->result);
    		} else { // is a range base and storage has been set by another thread
				 my_s = help_s;
			}
    	} else if(is_replaceable(b)) { // result field != NOT_SET
    		my_s = new rs<T>;
    		node<T>* n = new_range_base(b, lo, hi, my_s); // new range base with updated result storage

    		if(!try_replace(t, b, n)) goto find_first; // reset range query

    		replace_top(s, n);
    	} else if( b->type == range && b->hi >= hi) { // expand range query
    		return all_in_range(t, b->lo, b->hi, b->storage);
    	} else {
    		help_if_needed(t, b);
    		goto find_first;
    	}

    	while(true) { // Find remaining base nodes
	    	push(done, b); // ultimate final result stack
	    	backup_s = copy_state(s);
	    	if (!empty(b->data) && max(b->data) >= hi)
				break;

	    	find_next_base_node: b = find_next_base_stack(s);
	    	if(b == NULL) break; // out of base nodes
	    	else if (aload(&my_s->result) != NOT_SET) { // range query is finished
	    		return aload(&my_s->result);
	    	} else if (b->type == range && b->storage == my_s) { // b's storage is the same as the current
	    		continue;
	    	} else if (is_replaceable(b)) {
	    		node<T>* n = new_range_base(b, lo, hi, my_s); // change the type of node b is
	    		if(try_replace(t, b, n)) {
	    			replace_top(s, n);
                    continue;
	    		} else {
	    			s = copy_state(backup_s); // reset the stack
	    			goto find_next_base_node;
	    		}
	    	} else { // another thread has intercepted; help it out
	    		help_if_needed(t, b);
	    		s = copy_state(backup_s); // reset stack
	    		goto find_next_base_node;
	    	}
    	}

    	treap<T>* res = done->stack_array[0]->data; // stack array is just an array of nodes
    	for(int i = 1; i < done->size; i++)
    		res = treap_join(res, done->stack_array[i]->data); // join all the data in the base nodes together

    	if(CAS(&my_s->result, NOT_SET, res) && done->size > 1) // if still not set by another thread, replace
    		astore(&my_s->more_than_one_base , true);

    	adapt_if_needed(t, done->array[rand() % done->size]);
    	return aload(&my_s->result);
    }

    // Adaptations
    node<T>* deep_copy(node<T>* b) {
        node<T>* a = new node<T>*();
        a->data = b->data;
        a->stat = b->stat;
        a->parent = b->parent;
        a->lo = b->lo; a->hi = b->hi;
        a->storage = b->storage;
        a->neigh1 = b->neigh1;
        a->neigh2 = b->neigh2;
        a->gparent = b->gparent;
        a->otherb = b->otherb;
        a->main_node = b->main_node;

        return a;
    }

    // Adaptations
    node<T>* leftmost(node<T>* n) {
        while (n->type == route) {
            n = aload(&n->left);
        }
        return n;
    }

    // Adaptations
    node<T>* parent_of(lfcatree* t, node<T>* n) {
        node<T>* prev_node = NULL;
        node<T>* curr_node = aload(&t->root);

        while(curr_node != n && curr_node->type == route) {
            prev_node = curr_node;
            if(n->key < curr_node->key) {
                curr_node = aload(&curr_node->left);
            } else {
                curr_node = aload(&curr_node->right);
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
    node<T>* secure_join_left(lfcatree* t, node<T>* b) {
        node<T>* n0 = leftmost(aload(&b->parent->right)); // get the neighboring node on the right
        if(!is_replaceable(n0)) return NULL;

        node<T>* m = deep_copy(b); // m is the main node
        m->type = joinmain; // mark that it is part of a join

        if(!CAS(&b->parent->left, b, m)) return NULL; // check that it's still on the left side and replace it (b)

        node<T>* n1 = deep_copy(n0);
        n1->main_node = m; // copy the neighboring node to replace it and change its type to join (c)

        if(!try_replace(t, n0, n1)) { // replace the neighboring node
            astore(&m->neigh2, ABORTED);
            return NULL;
        } else if(!CAS(&m->parent->join_id, NULL, m)) { // check that another thread has not attatched
                                                        // a join id and if not, set it (d)
            astore(&m->neigh2, ABORTED);
            return NULL;
        }

        node<T>* gparent = parent_of(t, m->parent); // set the join ids of the parents and grandparents to
                                                    // indicate that it is part of a join
        if(gparent == NOT_FOUND ||
          (gparent != NULL && !CAS(&gparent->join_id,NULL,m))) {
            astore(&m->parent->join_id, NULL);
            return NULL;
        }

        m->gparent = gparent; // set the information used for complete_join
        m->otherb = aload(&m->parent->right); // set to the actual value of right neighbor
        m->neigh1 = n1; // set to the expected value of the right neighbor

        node<T>* joinedp = m->otherb==n1 ? gparent: n1->parent; // set the main node's neigh2 field to n2, which
                                                                // will eventually replace both m and n1 in the
                                                                // complete_join (e)
        node<T>* n2 = deep_copy(n1);
        n2->parent = joinedp;
        n2->main_node = m;
        n2->data = treap_join(m, n1); // TO-DO: join the data in the two nodes

        if(CAS(&m->neigh2, PREPARING, n2)) return m; // should end here if CAS is successful

        if(gparent == NULL) {
            astore(&m->parent->join_id, NULL);
            return NULL;
        }
        astore(&gparent->join_id, NULL);

        return NULL;
    }

    // Adaptations
    // The first phase of the join as described by the paper. Other threads
    // cannot help with this process. The corresponding diagrams are marked
    // on their place in the code.
    node<T>* secure_join_right(lfcatree* t, node<T>* b) {
        node<T>* n0 = rightmost(aload(&b->parent->left)); // get the neighboring node on the left
        if(!is_replaceable(n0)) return NULL;

        node<T>* m = deep_copy(b); // m is the main node
        m->type = joinmain; // mark that it is part of a join

        if(!CAS(&b->parent->right, b, m)) return NULL; // check that it's still on the right side and replace it (b)

        node<T>* n1 = deep_copy(n0);
        n1->main_node = m; // copy the neighboring node to replace it and change its type to join (c)

        if(!try_replace(t, n0, n1)) { // replace the neighboring node
            astore(&m->neigh2, ABORTED);
            return NULL;
        } else if(!CAS(&m->parent->join_id, NULL, m)) { // check that another thread has not attatched
                                                        // a join id and if not, set it (d)
            astore(&m->neigh2, ABORTED);
            return NULL;
        }

        node<T>* gparent = parent_of(t, m->parent); // set the join ids of the parents and grandparents to
                                                    // indicate that it is part of a join
        if(gparent == NOT_FOUND ||
          (gparent != NULL && !CAS(&gparent->join_id,NULL,m))) {
            astore(&m->parent->join_id, NULL);
            return NULL;
        }

        m->gparent = gparent; // set the information used for complete_join
        m->otherb = aload(&m->parent->left); // set to the actual value of left neighbor
        m->neigh1 = n1; // set to the expected value of the left neighbor

        node<T>* joinedp = m->otherb==n1 ? gparent: n1->parent; // set the main node's neigh2 field to n2, which
                                                                // will eventually replace both m and n1 in the
                                                                // complete_join (e)
        node<T>* n2 = deep_copy(n1);
        n2->parent = joinedp;
        n2->main_node = m;
        n2->data = treap_join(m, n1); // TO-DO: join the data in the two nodes

        if(CAS(&m->neigh2, PREPARING, n2)) return m; // should end here if CAS is successful

        if(gparent == NULL) {
            astore(&m->parent->join_id, NULL);
            return NULL;
        }
        astore(&gparent->join_id, NULL);

        return NULL;
    }

    // Adaptation
    // The second part of the join. Multiple threads can help out this
    // part of the join.
    void complete_join(lfcatree* t, node<T>* m) {
        node<T>* n2 = aload(&m->neigh2);

        if(n2 == DONE) return;

        try_replace(t, m->neigh1, n2); // replace the neighbor node (f)
        astore(&m->parent->valid, false); // mark the main node's parent as false to prevent other threads from
                                          // traversing to it
        node<T>* replacement = (m->otherb == m->neigh1) ? n2 : m->otherb; // check that the neighbor node that was
                                                                          // replaced wasn't changed by another thread
        if (m->gparent == NULL) { // parent is spliced out in the following condition statements (g)
            CAS(&t->root, m->parent, replacement); // replacement is the node with the merged data
        } else if(aload(&m->gparent->left) == m->parent) {
            CAS(&m->gparent->left, m->parent, replacement);
            CAS(&m->gparent->join_id, m, NULL);
        } else if(aload(&m->gparent->right) == m->parent) {
            CAS(&m->gparent->right, m->parent, replacement);
            CAS(&m->gparent->join_id, m, NULL);
        }
        astore(&m->neigh2, DONE); // n2 is now marked as replacable and the join has been completed (h)
    }

    // Adaptations
    // Join the contents of two base nodes into one base node
    void low_contention_adaptation(lfcatree* t, node<T>* b) {
        if(b->parent == NULL) return;
        if(aload(&b->parent->left) == b) { // check what side the node is on
            node<T>* m = secure_join_left(t, b);
            if (m != NULL) complete_join(t, m);
        } else if (aload(&b->parent->right) == b) { // check what side the node is on
            node<T>* m = secure_join_right(t, b);
            if (m != NULL) complete_join(t, m);
        }
    }

    // Adaptations
    // Get a suitable median value for the left and right base nodes' route node.
    int split_key(treap<T>* data) {
        return 0;
    }

    // Adaptations
    // Split the data in half less than the route node key's value.
    int split_left(treap<T>* data, int key) {
        return 0;
    }

    // Adaptations
    // Split the data in half greater than the route node key's value.
    int split_right(treap<T>* data, int key) {
        return 0;
    }

    // Adaptations
    // Split the contents of one base node into two base nodes
    void high_contention_adaptation(lfcatree* m, node<T>* b) {
        if(less_than_two_items(b->data)) return;
        node<T>* r = new node<T>*(); // create new route node to hold two new base nodes
        r->type = route;
        r->key = split_key(b->data); // TO-DO

        node<T>* left = new node<T>*();
        left->type = normal;
        left->parent = r;
        left->stat = 0;
        left->data = split_left(b->data, r->key); // TO-DO
        r->left = left;

        node<T>* right = new node<T>*();
        right->type = normal;
        right->parent = r;
        right->stat = 0;
        right->data = split_right(b->data, r->key); // TO-DO
        r->right = right;

        try_replace(m, b, r);
    }
};

int main () {
    std::cout << "Yo!! Yo!!" << std::endl;
    return 0;
}
