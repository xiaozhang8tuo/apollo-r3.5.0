# Transmitter

![Inheritance graph](assets/classapollo_1_1cyber_1_1transport_1_1Transmitter__inherit__graph.png)

# Transmitter

子类需要实现 **Enable Disable Transmit** 等方法

```cpp
template <typename M>
bool Transmitter<M>::Transmit(const MessagePtr& msg) {
  msg_info_.set_seq_num(NextSeqNum());
  PerfEventCache::Instance()->AddTransportEvent(
      TransPerf::TRANS_FROM, attr_.channel_id(), msg_info_.seq_num()); //PerfEventCache? 优化/缓存的是什么
  return Transmit(msg, msg_info_);
}

virtual bool Transmit(const MessagePtr& msg);
virtual bool Transmit(const MessagePtr& msg, const MessageInfo& msg_info) = 0;
```

# 线程间

# IntraTransmitter

进程内(线程间)的消息传递

```cpp
uint64_t channel_id_;
IntraDispatcherPtr dispatcher_;
```

在enable时，赋值 dispatcher_，不同话题的transmitter所发消息会通过同一个dispatch\_分发下去

```cpp
dispatcher_ = IntraDispatcher::Instance();
```

在Transmit发布消息时，调用派发器的OnMessage接口

```cpp
dispatcher_->OnMessage(channel_id_, msg, msg_info);
```

# Dispatcher

![Inheritance graph](assets/classapollo_1_1cyber_1_1transport_1_1Dispatcher__inherit__graph.png)

```cpp
std::atomic<bool> is_shutdown_;
// key: channel_id of message
AtomicHashMap<uint64_t, ListenerHandlerBasePtr> msg_listeners_; //原子的hashmap
base::AtomicRWLock rw_lock_;
```



```cpp
using MessageListener =
    std::function<void(const std::shared_ptr<MessageT>&, const MessageInfo&)>;

// 由transmitter调用
template <typename MessageT>
void Dispatcher::AddListener(const RoleAttributes& self_attr,
                             const RoleAttributes& opposite_attr,
                             const MessageListener<MessageT>& listener)
{
    //先检查是不是Listener对应的channel_id已经加入了AtomicHashMap
    handler.reset(new ListenerHandler<MessageT>());
    msg_listeners_.Set(channel_id, handler);
    handler->Connect(self_attr.id(), listener);//*
}


// 由Receiver调用
void Dispatcher::AddListener(const RoleAttributes& self_attr,
                             const MessageListener<MessageT>& listener) 
    
void Dispatcher::RemoveListener(const RoleAttributes& self_attr)
```

[ListenerHandlerBase/ListenerHandler 注册机制的实现](#ListenerHandlerBase/ListenerHandler) 

## OnMessage

dispatch中**最重要的onMessage实现并不是一个虚函数**

# IntraDispatcher 

线程派发器 intra_dispatcher.h

根据 RawMessage/MessageT 派发到不同类型的消息。RawMessage序列化后体积小，MessageT无需反序列化，适用于不同场景。

可以看到OnMessage回调的时候其实是在writer线程中调用这些reader的回调的。

```cpp
template <typename MessageT>
void IntraDispatcher::OnMessage(uint64_t channel_id,
                                const std::shared_ptr<MessageT>& message,
                                const MessageInfo& message_info) {
  if (is_shutdown_.load()) {
    return;
  }
  ADEBUG << "intra on message, channel:"
         << common::GlobalData::GetChannelById(channel_id);
  ListenerHandlerBasePtr* handler_base = nullptr;
  if (msg_listeners_.Get(channel_id, &handler_base)) {
    if ((*handler_base)->IsRawMessage()) {
      auto handler =
          std::dynamic_pointer_cast<ListenerHandler<message::RawMessage>>(
              *handler_base);
      auto msg = std::make_shared<message::RawMessage>();
      message::SerializeToString(*message, &msg->message);
      handler->Run(msg, message_info);
    } else {
      auto handler =
          std::dynamic_pointer_cast<ListenerHandler<MessageT>>(*handler_base);
      if (handler == nullptr) {
        AERROR << "please ensure that readers with the same channel["
               << common::GlobalData::GetChannelById(channel_id)
               << "] in the same process have the same message type";
        return;
      }
      handler->Run(message, message_info);
    }
  }
}
```



# ListenerHandlerBase/ListenerHandler



![Inheritance graph](assets/classapollo_1_1cyber_1_1transport_1_1ListenerHandlerBase__inherit__graph.png)

ListenerHandler要实现Disconnect 方法

[ apollo 中的 signal  slot  && connect ](./base_signal.md)

## 成员变量

```cpp
using Message = std::shared_ptr<MessageT>;
using MessageSignal = base::Signal<const Message&, const MessageInfo&>;
using Listener = std::function<void(const Message&, const MessageInfo&)>;
using MessageConnection = base::Connection<const Message&, const MessageInfo&>;
using ConnectionMap = std::unordered_map<uint64_t, MessageConnection>;  

using SignalPtr = std::shared_ptr<MessageSignal>;
using MessageSignalMap = std::unordered_map<uint64_t, SignalPtr>;

// used for self_id
MessageSignal signal_;                                     // auto connection = signal_.Connect(listener);
ConnectionMap signal_conns_;  // key: self_id              // signal_conns_[self_id] = connection;

// used for self_id and oppo_id
MessageSignalMap signals_;  // key: oppo_id             // auto connection = signals_[oppo_id]->Connect(listener);
// key: oppo_id
std::unordered_map<uint64_t, ConnectionMap> signals_conns_; // signals_conns_[oppo_id][self_id] = connection;

base::AtomicRWLock rw_lock_;
```

## 成员函数

```cpp
void Connect(uint64_t self_id, const Listener& listener);
void Connect(uint64_t self_id, uint64_t oppo_id, const Listener& listener);

void Disconnect(uint64_t self_id) override;
void Disconnect(uint64_t self_id, uint64_t oppo_id) override;

void Run(const Message& msg, const MessageInfo& msg_info);
```

## Run

当消息来，根据消息的来源(即id)找到对应的signal，触发signal中的所有slot的回调

```cpp
template <typename MessageT>
void ListenerHandler<MessageT>::Run(const Message& msg,
                                    const MessageInfo& msg_info) {
  signal_(msg, msg_info);
  uint64_t oppo_id = msg_info.sender_id().HashValue();
  ReadLockGuard<AtomicRWLock> lock(rw_lock_);
  if (signals_.find(oppo_id) == signals_.end()) {
    return;
  }

  (*signals_[oppo_id])(msg, msg_info); //触发signal中的所有slot的回调
}
```



# 进程间

# ShmTransmitter



```cpp
SegmentPtr segment_;   //共享内存中写
uint64_t channel_id_; 
uint64_t host_id_;    
NotifierPtr notifier_; //通知器
```

## enable

```cpp
segment_ = std::make_shared<Segment>(channel_id_, WRITE_ONLY); //分配共享内存
notifier_ = NotifierFactory::CreateNotifier();                 //创建通知器
this->enabled_ = true;
```

## Transmit

对 [transport-shm](./transport-shm.md) 中的block进行操作

```cpp
struct WritableBlock {
  uint32_t index = 0;
  Block* block = nullptr;
  uint8_t* buf = nullptr;
};
```

```cpp
AcquireBlockToWrite //让WritableBlock指向内存中的shm
// 写入Message 和 MessageInfo
segment_->ReleaseWrittenBlock(wb); //释放锁
ReadableInfo readable_info(host_id_, wb.index, channel_id_); 
notifier_->Notify(readable_info); //通知 //host_id_ 往channel_id_ 的共享内存 index 中写入了数据
```







# Receiver

![Inheritance graph](assets/classapollo_1_1cyber_1_1transport_1_1Receiver__inherit__graph.png)

# Receiver

当有新消息来的时候，执行构造时传入的函数

```cpp
Receiver(const RoleAttributes& attr, const MessageListener& msg_listener);

template <typename M>
void Receiver<M>::OnNewMessage(const MessagePtr& msg,
                               const MessageInfo& msg_info) {
  if (msg_listener_ != nullptr) {
    msg_listener_(msg, msg_info, attr_);
  }
}
```

# IntraReceiver

### Enable

```cpp
template <typename M>
void IntraReceiver<M>::Enable() {
  if (this->enabled_) {
    return;
  }

  dispatcher_->AddListener<M>(
      this->attr_, std::bind(&IntraReceiver<M>::OnNewMessage, this,
                             std::placeholders::_1, std::placeholders::_2));
  this->enabled_ = true;
}
```









