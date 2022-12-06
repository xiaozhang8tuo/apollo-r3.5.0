## BoundedQueue ##

不可拷贝的有界的无锁队列

## 成员 ##

```cpp
alignas(CACHELINE_SIZE) std::atomic<uint64_t> head_ = {0};  //头尾的index以及commit的index
alignas(CACHELINE_SIZE) std::atomic<uint64_t> tail_ = {1};
alignas(64) std::atomic<uint64_t> commit_ = {1};

uint64_t pool_size_ = 0;
T* pool_ = nullptr;
std::unique_ptr<WaitStrategy> wait_strategy_ = nullptr;
volatile bool break_all_wait_ = false;
```

**alignas** 

https://en.cppreference.com/w/cpp/language/alignas

https://blog.csdn.net/audi2/article/details/39103733

改变一个数据类型的对齐属性。在例子中，tail_的对齐值变成64，意味着a的地址值必须能被64整除。可能被快速访问的原子变量。

无效的非零对齐，例如 alignas(3) 格式错误。1或者4的整数倍。

## Init ##

指定界限，预先分配好size+2的一块地址，inplace_new 的在每个地址上创建空的T

**要求T要有默认构造函数，并且可以拷贝构造**

```cpp
template <typename T>
bool BoundedQueue<T>::Init(uint64_t size, WaitStrategy* strategy) {
  // Head and tail each occupy a space
  pool_size_ = size + 2;
  pool_ = reinterpret_cast<T*>(std::calloc(pool_size_, sizeof(T)));
  if (pool_ == nullptr) {
    return false;
  }
  for (int i = 0; i < pool_size_; ++i) {
    new (&(pool_[i])) T();
  }
  wait_strategy_.reset(strategy);
  return true;
}

```

## Enqueue 入队列 ##

```cpp
template <typename T>
bool BoundedQueue<T>::Enqueue(const T& element) {
  uint64_t new_tail = 0;
  uint64_t old_commit = 0;
  uint64_t old_tail = tail_.load(std::memory_order_acquire);
  do {
    new_tail = old_tail + 1;
    if (GetIndex(new_tail) == GetIndex(head_.load(std::memory_order_acquire))) {
      return false;
    }
  } while (!tail_.compare_exchange_weak(old_tail, new_tail,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
  //如果tail_ == old_tail，说明只有自己更新，则 tail_ = new_tail, 返回true
  //如果tail_ != old_tial，说明还有其他人更新了，则 old_tail = tail，更新old_tail，返回false，再继续
  pool_[GetIndex(old_tail)] = element;
  do {
    old_commit = old_tail;
  } while (unlikely(!commit_.compare_exchange_weak(old_commit, new_tail,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)));
  wait_strategy_->NotifyOne();
  return true;
}
```

先原子的更新tail，再旧的tail上入队列。再原子的更新commit

后面的 unlikely(!commit_.compare_exchange_weak...)指的是：自己完成了原子更新tail的操作后，这次cas操作**大概率会成功**，请编译器尽情优化吧。

不会出现，A先更新tail，B再更新tail，然而B却比A更早提交的情况。保证了写入的有序性，同时保证了提交的有序性。commit永远+1+1的向前走。

## __builtin_expect ##

这个指令是gcc引入的，作用是**允许程序员将最有可能执行的分支告诉编译器**。这个指令的写法为：`__builtin_expect(EXP, N)`。
意思是：EXP==N的概率很大。

[__builtin_expect 说明](https://www.jianshu.com/p/2684613a300f)

```cpp
#if __GNUC__ >= 3
#define likely(x) (__builtin_expect((x), 1))
#define unlikely(x) (__builtin_expect((x), 0))
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif
```

## Dequeue 出队列 ##

```CPP
template <typename T>
bool BoundedQueue<T>::Dequeue(T* element) {
  uint64_t new_head = 0;
  uint64_t old_head = head_.load(std::memory_order_acquire);
  do {
    new_head = old_head + 1;
    if (new_head == commit_.load(std::memory_order_acquire)) {
      return false;
    }
    *element = pool_[GetIndex(new_head)];
  } while (!head_.compare_exchange_weak(old_head, new_head,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed));
  return true;
}
```

# 总结 #

- alignas [əˈlaɪnas] 内存对齐用法
- 写入再提交的思想和实现。
- likely && unlikely && __builtin_expect