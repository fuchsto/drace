
#include <mutex> // for lock_guard
#include <iostream>
#include <detector/Detector.h>
#include <ipc/spinlock.h>
#include "threadstate.h"
#include "varstate.h"
#include "xmap.h"
#include "xvector.h"



#define MAKE_OUTPUT false
#define REGARD_ALLOCS true



class Fasttrack : public Detector {

    /// globals ///
    static constexpr int max_stack_size = 16;

    xmap<size_t, size_t> allocs;
    xmap<size_t, VarState*> vars;
    xmap<void*, VectorClock*> locks;
    xmap<Detector::tid_t, std::shared_ptr<ThreadState>> threads;
    xmap<void*, VectorClock*> happens_states;
    void * clb;

    //declaring order is also the acquiring order of the locks
    ipc::spinlock t_lock;
    ipc::spinlock v_lock;
    ipc::spinlock l_lock;
    ipc::spinlock h_lock;
    ipc::spinlock a_lock;

    void report_race(
        unsigned thr1, unsigned thr2,
        bool wr1, bool wr2,
        size_t var,
        int clk1, int clk2)
    {
        uint32_t var_size = 0;

        VarState* var_object = vars[var];
        var_size = var_object->size;

        std::shared_ptr<ThreadState> thread1 = threads[thr1];
        std::shared_ptr<ThreadState> thread2 = threads[thr2];
        xvector<size_t> stack1, stack2;

        stack1 = thread1->return_stack_trace(var);
        stack2 = thread2->return_stack_trace(var);

        if (stack1.size() > 16) {
            auto end_stack = stack1.end();
            auto begin_stack = stack1.end() - 16;

            stack1 = xvector<size_t>(begin_stack, end_stack);
        }
        if (stack2.size() > 16) {
            auto end_stack = stack2.end();
            auto begin_stack = stack2.end() - 16;

            stack2 = xvector<size_t>(begin_stack, end_stack);
        }

        Detector::AccessEntry access1;
        access1.thread_id = thr1;
        access1.write = wr1;
        access1.accessed_memory = var;
        access1.access_size = var_size;
        access1.access_type = 0;
        access1.heap_block_begin = 0;
        access1.heap_block_size = 0;
        access1.onheap = false;
        access1.stack_size = stack1.size();
        std::copy(stack1.begin(), stack1.end(), access1.stack_trace);

        Detector::AccessEntry access2;
        access2.thread_id = thr2;
        access2.write = wr2;
        access2.accessed_memory = var;
        access2.access_size = var_size;
        access2.access_type = 0;
        access2.heap_block_begin = 0;
        access2.heap_block_size = 0;
        access2.onheap = false;
        access2.stack_size = stack2.size();
        std::copy(stack2.begin(), stack2.end(), access2.stack_trace);


        Detector::Race race;
        race.first = access1;
        race.second = access2;


        ((void(*)(const Detector::Race*))clb)(&race);
    }



    void read(std::shared_ptr<ThreadState> t, VarState* v) {
        //cannot happen in current implementation as rw increments clock, discuss
        if (t->get_self() == v->r_clock && t->tid == v->get_r_tid()) {//read same epoch, sane thread
            return;
        }

        if (v->r_clock == VarState::READ_SHARED && v->get_vc_by_id(t) == t->get_self() && v->get_vc_by_id(t) != 0) { //read shared same epoch
            return;
        }

        if (v->is_wr_race(t))
        { // write-read race
            report_race(v->get_w_tid(), t->tid, true, false, v->address, v->w_clock, t->get_self());
        }

        //update vc
        if (v->r_clock != VarState::READ_SHARED)
        {
            if (v->r_clock == VarState::VAR_NOT_INIT ||
                (v->get_r_tid() == t->tid     &&
                    v->r_clock < t->get_vc_by_id(v->get_r_tid())))//read exclusive
            {
                v->update(false, t);
            }
            else { // read gets shared
                v->set_read_shared(t);
            }
        }
        else {//read shared
            v->update(false, t);
        }

    }

    void write(std::shared_ptr<ThreadState> t, VarState* v) {

        if (v->w_clock == VarState::VAR_NOT_INIT) { //initial write, update var
            v->update(true, t);
            return;
        }

        // not possib with current impl
        if (t->get_self() == v->w_clock &&
            t->tid == v->get_w_tid())
        {//write same epoch
            return;
        }

        //tids are different && and write epoch greater or equal than known epoch of other thread
        if (v->is_ww_race(t)) // write-write race
        {
            report_race(v->get_w_tid(), t->tid, true, true, v->address, v->w_clock, t->get_self());
        }

        if (v->r_clock != VarState::READ_SHARED) {
            if (v->is_rw_ex_race(t))// read-write race
            {
                report_race(v->get_r_tid(), t->tid, false, true, v->address, v->r_clock, t->get_self());
            }
        }
        else {//come here in read shared case
            std::shared_ptr<ThreadState> act_thr = v->is_rw_sh_race(t);
            if (act_thr != nullptr) //read shared read-write race       
            {
                report_race(act_thr->tid, t->tid, false, true, v->address, v->get_vc_by_id(act_thr), t->get_self());
            }
        }
        v->update(true, t);
    }

    void create_var(size_t addr, size_t size) {
        VarState* var = new VarState(addr, size);
        v_lock.lock();
        vars.insert(vars.end(), std::pair<size_t, VarState*>(addr, var));
        v_lock.unlock();
    }

    void create_lock(void* mutex) {
        VectorClock* lock = new VectorClock();
        std::lock_guard<ipc::spinlock>lg_v(v_lock);
        locks.insert(locks.end(), std::pair<void*, VectorClock*>(mutex, lock));
    }

    ///the deleter for a ThreadStates Object
    ///if it is deleted, also the tid is removed of all other vector clocks which hold the tid

    void create_thread(Detector::tid_t tid, std::shared_ptr<ThreadState> parent = nullptr) {

        std::shared_ptr<ThreadState> new_thread;
        if (parent == nullptr) {
            new_thread = std::make_shared<ThreadState>(this, tid);
        }
        else {
            std::lock_guard<ipc::spinlock>lg_t(t_lock);
            new_thread = std::make_shared<ThreadState>(this, tid, parent);
        }
        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        threads.insert(threads.end(), { tid, new_thread });

    }



    void create_happens(void* identifier) {
        VectorClock * new_hp = new VectorClock();
        std::lock_guard<ipc::spinlock>lg_h(h_lock);
        happens_states.insert(happens_states.end(), std::pair<void*, VectorClock*>(identifier, new_hp));
    }

    void create_alloc(size_t addr, size_t size) {
        std::lock_guard<ipc::spinlock>lg_a(a_lock);
        allocs.insert(allocs.end(), { addr, size });
    }

public:

    void cleanup(uint32_t tid) {
        //as ThreadState is destroyed delete all the entries from all vectorclocks
        l_lock.lock();
        for (auto it = locks.begin(); it != locks.end(); ++it) {
            it->second->delete_vc(tid);
        }
        l_lock.unlock();
        t_lock.lock();
        for (auto it = threads.begin(); it != threads.end(); ++it) {
            it->second->delete_vc(tid);
        }
        t_lock.unlock();
        h_lock.lock();
        for (auto it = happens_states.begin(); it != happens_states.end(); ++it) {
            it->second->delete_vc(tid);
        }
        h_lock.unlock();
    }

    bool init(int argc, const char **argv, Callback rc_clb) {
        clb = rc_clb; //init callback
        return true;
    }

    void finalize() {

        while (vars.size() > 0) {
            auto it = vars.begin();
            delete it->second;
            vars.erase(it);
        }
        while (locks.size() > 0) {
            auto it = locks.begin();
            delete it->second;
            locks.erase(it);
        }
        while (threads.size() > 0) {
            threads.erase(threads.begin());
        }
        while (happens_states.size() > 0) {
            auto it = happens_states.begin();
            delete it->second;
            happens_states.erase(it);
        }
    }


    void read(tls_t tls, void* pc, void* addr, size_t size) {
        size_t var_address = reinterpret_cast<size_t>(addr);

        if (vars.find(var_address) == vars.end()) { //create new variable if new
#if MAKE_OUTPUT
            std::cout << "variable is read before written" << std::endl;//warning
#endif
            create_var(var_address, size);
        }

        auto thr = threads[reinterpret_cast<Detector::tid_t>(tls)];

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        {
            std::lock_guard<ipc::spinlock>lg_v(v_lock);

            thr->set_read_write(var_address, reinterpret_cast<size_t>(pc));
            read(thr, vars[var_address]);
        }
    }

    void write(tls_t tls, void* pc, void* addr, size_t size) {
        size_t var_address = ((size_t)addr);

        if (vars.find(var_address) == vars.end()) {
            create_var(var_address, size);
        }
        auto thr = threads[reinterpret_cast<Detector::tid_t>(tls)];

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        {
            std::lock_guard<ipc::spinlock>lg_v(v_lock);

            thr->set_read_write(var_address, reinterpret_cast<size_t>(pc));
            write(thr, vars[var_address]);
        }
    }


    void func_enter(tls_t tls, void* pc) {
        std::shared_ptr<ThreadState> thr = threads[reinterpret_cast<Detector::tid_t>(tls)];
        size_t stack_element = reinterpret_cast<size_t>(pc);

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        thr->push_stack_element(stack_element);

    }

    void func_exit(tls_t tls) {
        std::shared_ptr<ThreadState> thr = threads[reinterpret_cast<Detector::tid_t>(tls)];

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        thr->pop_stack_element(); //pops last stack element of current clock
    }


    void fork(tid_t parent, tid_t child, tls_t * tls) {
        *tls = reinterpret_cast<void*>(child);

        if (threads.find(parent) != threads.end()) {
            t_lock.lock();
            threads[parent]->inc_vc(); //inc vector clock for creation of new thread
            t_lock.unlock();
            create_thread(child, threads[parent]);
        }
        else {
            create_thread(child);
        }

    }

    void join(tid_t parent, tid_t child) {
        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        std::shared_ptr<ThreadState> del_thread = threads[child];
        del_thread->inc_vc();

        //pass incremented clock of deleted thread to parent
        auto ptr = del_thread.get();
        threads[parent]->update(ptr);

        threads.erase(child);
    }


    void acquire(tls_t tls, void* mutex, int recursive, bool write) {

        if (locks.find(mutex) == locks.end()) {
            create_lock(mutex);
        }

        auto id = reinterpret_cast<Detector::tid_t>(tls);

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        {
            std::lock_guard<ipc::spinlock>lg_l(l_lock);
            //propagate sync data from lock to thread
            (threads[id])->update(locks[mutex]);
        }

    }

    void release(tls_t tls, void* mutex, bool write) {

        if (locks.find(mutex) == locks.end()) {
#if MAKE_OUTPUT
            std::cerr << "lock is released but was never acquired by any thread" << std::endl;
#endif
            create_lock(mutex);
        }

        auto id = reinterpret_cast<Detector::tid_t>(tls);

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        {
            std::lock_guard<ipc::spinlock>lg_l(l_lock);
            //increase vector clock and propagate to lock
            threads[id]->inc_vc();
            auto ptr = (threads[id]).get();
            (locks[mutex])->update(ptr);
        }
    }


    void happens_before(tls_t tls, void* identifier) {
        if (happens_states.find(identifier) == happens_states.end()) {
            create_happens(identifier);
        }

        VectorClock* hp = happens_states[identifier];
        std::shared_ptr<ThreadState> thr = threads[reinterpret_cast<Detector::tid_t>(tls)];

        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        {
            std::lock_guard<ipc::spinlock>lg_h(h_lock);
            //increment clock of thread and update happens state
            thr->inc_vc();
            hp->update(thr->tid, thr->get_self());
        }
    }

    /** Draw a happens-after edge between thread and identifier (optional) */
    void happens_after(tls_t tls, void* identifier) {

        if (happens_states.find(identifier) != happens_states.end()) {
            VectorClock* hp = happens_states[identifier];
            std::shared_ptr<ThreadState> thr = threads[reinterpret_cast<Detector::tid_t>(tls)];

            std::lock_guard<ipc::spinlock>lg_t(t_lock);
            {
                std::lock_guard<ipc::spinlock>lg_h(h_lock);
                //update vector clock of thread with happened before clocks
                thr->update(hp);
            }

        }
        else {//create -> no happens_before can be synced
            create_happens(identifier);
        }


    }


    void allocate(
        /// ptr to thread-local storage of calling thread
        tls_t  tls,
        /// current instruction pointer
        void*  pc,
        /// begin of allocated memory block
        void*  addr,
        /// size of memory block
        size_t size
    ) {
#if REGARD_ALLOCS
        size_t address = reinterpret_cast<size_t>(addr);
        if (allocs.find(address) == allocs.end()) {
            create_alloc(address, size);
        }
#endif
    }

    /** Log a memory deallocation*/
    void deallocate(
        /// ptr to thread-local storage of calling thread
        tls_t tls,
        /// begin of memory block
        void* addr
    ) {
#if REGARD_ALLOCS
        size_t address = reinterpret_cast<size_t>(addr);
        size_t size = allocs[address];
        size_t end_addr = address + size;

        //variable is deallocated so varstate objects can be destroyed
        while (address < end_addr) {
            if (vars.find(address) != vars.end()) {
                std::lock_guard<ipc::spinlock> lg_v(v_lock);
                {
                    VarState* var = vars.find(address)->second;
                    size_t var_size = var->size;
                    address += var_size;
                    delete var;
                    vars.erase(address);
                }
            }
            else {
                address++;
            }
        }
        std::lock_guard<ipc::spinlock> lg_a(a_lock);
        allocs.erase(address);

#endif  
    }


    void detach(tls_t tls, tid_t thread_id) {
        /// \todo This is currently not supported
        return;
    }
    /** Log a thread exit event of a detached thread) */
    void finish(tls_t tls, tid_t thread_id) {
        ///just delete thread from list, no backward sync needed
        std::lock_guard<ipc::spinlock>lg_t(t_lock);
        threads.erase(thread_id);
    }

    void map_shadow(void* startaddr, size_t size_in_bytes) {
        return;
    }


    const char * name() {
        return "FASTTRACK";
    }

    const char * version() {
        return "0.0.1";
    }
};



extern "C" __declspec(dllexport) Detector * CreateDetector() {
    return new Fasttrack();
}
