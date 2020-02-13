#include<vector>
#include<queue>
#include<memory>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<future>
#include<functional>
#include<stdexcept>

class threadPool
{
public:
    threadPool(size_t);
    
    template<class F,class... Args>
    auto enqueue(F&&f , Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

    ~threadPool();



private:
    //用于处理任务的线程
    std::vector<std::thread> workers;

    //任务队列
    std::queue<std::function<void()>> tasks;

    //用于控制修改任务队列的锁
    std::mutex  queue_mtx;

    //
    std::condition_variable cv;

    bool stop;


};

//使用inline修饰副，将此函数表示为内联函数。内联函数用于解决函数的多次调用导致的栈空间的消耗。inline修饰符只有与函数的定义一起才能发挥作用，例如此处
//类成员函数的定义加上字段inline。
inline threadPool::threadPool(size_t thread_num) : stop(false)//线程池的构造函数创建了一定数量的线程（每一个线程执行的都是emplace_back函数中的lambda表达式）
{                                                                                                                            //使用lambda表达式作为每个线程的任务。每个线程的任务大致如下：
    for(size_t i = 0; i < thread_num; i++ )                                                //创建一个function对象，然后从人物队列tasks中获取任务，如果此时任务队列中没有任务，则等待任务的到来
    {                                                                                                                        //同时释放对任务队列tasks的所有权（wait函数在未返回时会释放lck锁）
        workers.emplace_back(                                                                    //在队列中有任务且获得了锁的前提下，从队列中获取任务并执行。
            [this]
            {
                for(;;)//只要线程池还存在，当前的线程就不断循环从队列中获取任务，没有任务则进入while循环等待
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lck(queue_mtx);
                        while( stop  ==  false  &&  tasks.empty() == true)
                        {
                            cv.wait(lck);                                                                               //当wait函数阻塞了当前的线程时，该函数会自动的调用lck.unlock()释放锁，使其他的竞争者可以获得锁
                        }                                                                                                           //假设当前的任务队列为空，则当前的线程会阻塞在这个循环处，此时释放了lck锁使得其他的竞争者可以
                                                                                                                                    //获得队列的使用权，比如可以向队列中加入新的任务。当这个线程被唤醒时（被通知），wait函数又会自动的调用lock函数
                        if(stop == true && tasks.empty() == true)                          //对队列上锁。
                        {
                            return;
                        }

                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();

                }
            }
        );
    }
}

//向线程池的任务队列添加新的任务，由于无法确定具体的任务函数的类型和参数，所以此处使用泛型
//此泛型中，F是函数名，...Args是函数的参数，返回类型被尾置，std::result_of可以在编译的时候推导出一个函数表达式的返回值类型
template<class F,class... Args>
auto threadPool::enqueue(F&&f , Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>
{
    //将任务的返回子类型统称为return_type，return_type的值由result_of确定
    using  return_type = typename std::result_of<F(Args...)>::type;

    //此处用make_shared创建一个智能指针，该智能指针的类型是一个packaged_task。
    //packaged_task包装一个可调用的对象，此处调用std::bind()将泛型中的调用对象F和参数args组合成了一个可调用对象，赋值给packaged_task
    auto task = std::make_shared<std::packaged_task<return_type()>> (std::bind(std::forward<F>(f) , std::forward<Args> (args)...));

    std::future<return_type> res = task->get_future();

    {//由于对任务队列的操作必须是互斥的，所以要先获得对队列操作的锁
        std::unique_lock<std::mutex> lck(queue_mtx);
        if(stop)
        {
            throw std::runtime_error("enqueue on stopped threadpool");
        }

        tasks.emplace([task](){(*task)();});//将任务压入任务队列



     }
    //唤醒一个等待中的线程
    cv.notify_one();
    return res;
}


inline threadPool::~threadPool()
{
    {
        std::unique_lock<std::mutex> lck(queue_mtx);
        stop = true;
    }
    //析构线程池，将标志位stop设置为true，通知所有的线程，此时当任务队列没有任务时，构造函数直接进入return的分支
    cv.notify_all();
    for(auto & u:workers)
        u.join();
}