



# ConditionNotifier/ReadableInfo

![Collaboration graph](./.assets/classapollo_1_1cyber_1_1transport_1_1ConditionNotifier__coll__graph.png)



ConditionNotifier继承自基类NotifierBase，实现了Notify，Listen，Shutdown等方法。**配合is_shutdown_标志位操作条件变量**，达到进程间同步

Notify()  加锁，写入数据，写指针前移，notify_all

Listen()  加锁，wait_for，读取数据，读指针前移

Shutdown()  shutdown标志位

![image-20221008214815597](.assets/image-20221008214815597.png)

- 不同的进程中的同一个单例，指向**相同的共享内存地址**(进程间通信)。

  构造时的**OpenOrCreate**，操作共享内存shmget、shmat、shmctl、shmdt

- 该共享内存用于进程同步的方式是**条件变量和互斥锁**， **PTHREAD_PROCESS_SHARED**(进程间同步)。**多个消费者互不影响**

  ```cpp
  pthread_mutexattr_t mtx_attr;
  pthread_mutexattr_init(&mtx_attr);
  pthread_mutexattr_setpshared(&mtx_attr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(indicator_->mtx.native_handle(), &mtx_attr);
  
  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
  pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(indicator_->cv.native_handle(), &cond_attr);
  ```

- 写指针只有一个，读指针在不同进程中不同。**类似ringbuffer**中的读写指针。



问题：

1 数据以什么形式存放，readableinfo中只是一个索引

2  Notify，Listen 由谁调用

# Segment

![Collaboration graph](assets/classapollo_1_1cyber_1_1transport_1_1Segment__coll__graph.png)

## 成员变量

```cpp
bool init_; //分配标志
key_t id_;  //channel_id
ReadWriteMode mode_;//读写模式
ShmConf conf_;
State* state_; 
Block* blocks_;
void* managed_shm_;//shm的起点

std::mutex block_buf_lock_;
std::unordered_map<uint32_t, uint8_t*> block_buf_addrs_;

// STATE_SIZE + Block[block_num] + [消息+消息头] * block_num
// state*       blocks*            uint8_t*|uint8_t*|uint8_t*|uint8_t*
```

## Init

```cpp
bool Segment::Init() {
  if (mode_ == READ_ONLY) {
    return OpenOnly();
  } else {
    return OpenOrCreate();
  }
}
```

## OpenOrCreate/OpenOnly

两者之间只差一个shmget 的 IPC_CREAT操作

```cpp
shmid = shmget(id_, conf_.managed_shm_size(), 0644 | IPC_CREAT | IPC_EXCL);//分配
managed_shm_ = shmat(shmid, nullptr, 0);//attach
state_ = new (managed_shm_) State(conf_.ceiling_msg_size());
blocks_ = new (static_cast<char*>(managed_shm_) + sizeof(State)) Block[conf_.block_num()];

// create block buf
uint32_t i = 0;
for (; i < conf_.block_num(); ++i) {
  uint8_t* addr =
      new (static_cast<char*>(managed_shm_) + sizeof(State) +
           conf_.block_num() * sizeof(Block) + i * conf_.block_buf_size())
          uint8_t[conf_.block_buf_size()];
  std::lock_guard<std::mutex> _g(block_buf_lock_);
  block_buf_addrs_[i] = addr;
}

//EXTRA_SIZE + STATE_SIZE + Block[block_num] + [消息+消息头] * block_num
```

## ShmConf

描述一块共享内存

```cpp
GetCeilingMessageSize //一条消息的大小,取整
GetBlockBufSize       //消息+消息头(sender_id) = 内存块大小
GetBlockNum           //多少个内存块
managed_shm_size_ =
      EXTRA_SIZE + STATE_SIZE + (BLOCK_SIZE + block_buf_size_) * block_num_;
    //EXTRA_SIZE + STATE_SIZE +  [|len|消息+消息头|] * block_num
// 开辟这么大，使用的时候，内存分布不是这样的 OpenOrCreate
```

## State

```cpp
std::atomic<bool> need_remap_ = {false};
std::atomic<uint32_t> wrote_num_ = {0};      // 原子的增加wrote_num
std::atomic<uint32_t> reference_count_ = {0};// 原子的Increase/Decrease ReferenceCounts引用计数
std::atomic<uint64_t> ceiling_msg_size_;
```

## Block

```cpp
const int32_t Block::kRWLockFree = 0;      // 改block无人读写
const int32_t Block::kWriteExclusive = -1; // 该block正在写
const int32_t Block::kMaxTryLockTimes = 5; // 最大尝试次数

volatile std::atomic<int32_t> lock_num_ = {0};

uint64_t msg_size_;
uint64_t msg_info_size_;
```

获取写锁置为-1，获取读锁++。

写锁获取过程中，获取失败(lock_num_!=0)就返回

读锁获取过程中，可多次尝试，如果锁正在被写，则返回

### TryLockForWrite

```cpp
int32_t rw_lock_free = kRWLockFree;//0 //无人读取才能获取到写锁
if (!lock_num_.compare_exchange_weak(rw_lock_free, kWriteExclusive,
                                     std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) {
  ADEBUG << "lock num: " << lock_num_.load();
  return false;
}
return true;
```

### TryLockForRead

```cpp
int32_t lock_num = lock_num_.load();
if (lock_num < kRWLockFree) {
  AINFO << "block is being written.";
  return false;
}
int32_t try_times = 0;
while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
  ++try_times;
  if (try_times == kMaxTryLockTimes) {
    AINFO << "fail to add read lock num, curr num: " << lock_num;
    return false;
  }
  lock_num = lock_num_.load();
  if (lock_num < kRWLockFree) {
    AINFO << "block is being written.";
    return false;
  }
}
return true;
```

### ReleaseWriteLock/ReleaseReadLock

```cpp
void Block::ReleaseWriteLock() { lock_num_.fetch_add(1); }
void Block::ReleaseReadLock() { lock_num_.fetch_sub(1); }
```



