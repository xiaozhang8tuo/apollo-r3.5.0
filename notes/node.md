# Node

![Collaboration graph](assets/classapollo_1_1cyber_1_1Node__coll__graph.png)

## 成员变量

```cpp
std::string node_name_; //初始化时设置
std::string name_space_;//初始化时设置

std::mutex readers_mutex_;
std::map<std::string, std::shared_ptr<ReaderBase>> readers_; //管理自己的订阅者

std::unique_ptr<NodeChannelImpl> node_channel_impl_ = nullptr;
std::unique_ptr<NodeServiceImpl> node_service_impl_ = nullptr;
```

## 构造

```cpp
Node::Node(const std::string& node_name, const std::string& name_space)
    : node_name_(node_name), name_space_(name_space) {
  node_channel_impl_.reset(new NodeChannelImpl(node_name));
  node_service_impl_.reset(new NodeServiceImpl(node_name));
}
```





# NodeChannelImpl

话题发布订阅的内部实现

## 成员变量

![Collaboration graph](assets/classapollo_1_1cyber_1_1NodeChannelImpl__coll__graph.png)

```cpp
bool is_reality_mode_;
std::string node_name_;
proto::RoleAttributes node_attr_;
NodeManagerPtr node_manager_ = nullptr; 
//using NodeManagerPtr = std::shared_ptr<service_discovery::NodeManager>; //服务发现中的节点管理器
node_manager_->Join(node_attr_, RoleType::ROLE_NODE);
```



node的属性

```proto
message RoleAttributes {
    optional string host_name = 1; //node_attr_.set_host_name(common::GlobalData::Instance()->HostName());
    optional string host_ip = 2;
    optional int32 process_id = 3;
    optional string node_name = 4;
    optional uint64 node_id = 5;           // hash value of node_name
    // especially for WRITER and READER
    optional string channel_name = 6;
    optional uint64 channel_id = 7;        // hash value of channel_name
    optional string message_type = 8;
    optional bytes proto_desc = 9;
    optional uint64 id = 10;
    optional QosProfile qos_profile = 11;
    optional SocketAddr socket_addr = 12;  // reserved for socket communication
    // especially for SERVER and CLIENT
    optional string service_name = 13;
    optional uint64 service_id = 14;       // hash value of service_name
};
```



## CreateReader

[IntraReader 和 IntraWriter ](./blocker.md)

```cpp
template <typename MessageT>
auto NodeChannelImpl::CreateReader(const proto::RoleAttributes& role_attr,
                                   const CallbackFunc<MessageT>& reader_func,
                                   uint32_t pending_queue_size)
    -> std::shared_ptr<Reader<MessageT>> {
  if (!role_attr.has_channel_name() || role_attr.channel_name().empty()) {
    AERROR << "Can't create a reader with empty channel name!";
    return nullptr;
  }

  proto::RoleAttributes new_attr(role_attr);
  FillInAttr<MessageT>(&new_attr);

  std::shared_ptr<Reader<MessageT>> reader_ptr = nullptr;
  //根据模式不同创建 IntraReader 和 Reader  //blocker::IntraReader
  if (!is_reality_mode_) {
    reader_ptr =
        std::make_shared<blocker::IntraReader<MessageT>>(new_attr, reader_func);
  } else {
    reader_ptr = std::make_shared<Reader<MessageT>>(new_attr, reader_func,
                                                    pending_queue_size);
  }

  RETURN_VAL_IF_NULL(reader_ptr, nullptr);
  RETURN_VAL_IF(!reader_ptr->Init(), nullptr);
  return reader_ptr;
}
```



## CreateWriter

```cpp
auto NodeChannelImpl::CreateWriter(const std::string& channel_name)
    -> std::shared_ptr<Writer<MessageT>>
{
	proto::RoleAttributes role_attr;
	role_attr.set_channel_name(channel_name); //给其设置channel_name
}

auto NodeChannelImpl::CreateWriter(const proto::RoleAttributes& role_attr)
    -> std::shared_ptr<Writer<MessageT>>
{
    //检查是否有channel_name字段
    FillInAttr<MessageT>(&new_attr); //copy节点的属性，host ip pid等属性是通用的
    
    //根据模式不同创建 IntraWriter 和 Writer  //blocker::IntraWriter
    if (!is_reality_mode_) 
    {
    	writer_ptr = std::make_shared<blocker::IntraWriter<MessageT>>(new_attr);
  	} else {
    	writer_ptr = std::make_shared<Writer<MessageT>>(new_attr);
    }
}



```



# NodeServiceImpl

## CreateService

## CreateClient

# Writer/WriterBase

![Inheritance graph](assets/classapollo_1_1cyber_1_1WriterBase__inherit__graph.png)

## WriterBase

子类要实现 Init Shutdown HasReader GetReaders等方法，内置一把锁，同时纪录proto::RoleAttributes属性(主要是channel)

```cpp
class WriterBase {
 public:
  explicit WriterBase(const proto::RoleAttributes& role_attr)
      : role_attr_(role_attr), init_(false) {}
  virtual ~WriterBase() {}

  virtual bool Init() = 0;
  virtual void Shutdown() = 0;

  virtual bool HasReader() { return false; }
  virtual void GetReaders(std::vector<proto::RoleAttributes>* readers) {}

  const std::string& GetChannelName() const {
    return role_attr_.channel_name();
  }

  bool IsInit() const { std::lock_guard<std::mutex> g(lock_); return init_; }

 protected:
  proto::RoleAttributes role_attr_;
  mutable std::mutex lock_;
  bool init_;
};
```

## Writer

### 成员变量

```cpp
using TransmitterPtr = std::shared_ptr<transport::Transmitter<MessageT>>;
using ChangeConnection = typename service_discovery::Manager::ChangeConnection;

TransmitterPtr transmitter_; //发消息

ChangeConnection change_conn_;//自己用来和channel_manager_交互的信息
service_discovery::ChannelManagerPtr channel_manager_;//channel_manager_ (发布订阅的加入退出以及通知各自)
```

### * Init

```cpp
template <typename MessageT>
bool Writer<MessageT>::Init() {
  {
    std::lock_guard<std::mutex> g(lock_);
    if (init_) { return true; }
    transmitter_ = transport::Transport::Instance()->
      CreateTransmitter<MessageT>(role_attr_); //根据消息类型和节点属性创建收发器
    if (transmitter_ == nullptr) { return false; }
    init_ = true;
  }
  this->role_attr_.set_id(transmitter_->id().HashValue()); //role 中的id设置为收发器的id
  //service_discovery::TopologyManager 返回服务发现TopologyManager中的channel_manager
  channel_manager_ = service_discovery::TopologyManager::Instance()-> 
    channel_manager();
  JoinTheTopology();
  return true;
}
```

### write 

发送消息

```cpp
template <typename MessageT>
bool Writer<MessageT>::Write(const MessageT& msg) {
  RETURN_VAL_IF(!WriterBase::IsInit(), false);
  auto msg_ptr = std::make_shared<MessageT>(msg);
  return Write(msg_ptr);
}

template <typename MessageT>
bool Writer<MessageT>::Write(const std::shared_ptr<MessageT>& msg_ptr) {
  RETURN_VAL_IF(!WriterBase::IsInit(), false);
  return transmitter_->Transmit(msg_ptr);
}
```

### JoinTheTopology

```cpp
// 1 监听channel_manager_中的变化信息(粉丝的变化)，回调到自己的OnChannelChange
change_conn_ = channel_manager_->AddChangeListener(
    std::bind(&Writer<MessageT>::OnChannelChange, this, std::placeholders::_1));
// 2 get peer readers.读写者的创建并没有先后顺序，自己创建之前就订阅自己的读者在本阶段加入。
const std::string& channel_name = this->role_attr_.channel_name();
std::vector<proto::RoleAttributes> readers;
channel_manager_->GetReadersOfChannel(channel_name, &readers);
for (auto& reader : readers) {
  transmitter_->Enable(reader);
}
//3 作为发布者加入
channel_manager_->Join(this->role_attr_, proto::RoleType::ROLE_WRITER,
                         message::HasSerializer<MessageT>::value);
```

### LeaveTheTopology

```cpp
channel_manager_->RemoveChangeListener(change_conn_); //不再监听变化
channel_manager_->Leave(this->role_attr_, proto::RoleType::ROLE_WRITER);//退出
```



### OnChannelChange

只监听订阅者的变化，收发器发/不发订阅者

```cpp
template <typename MessageT>
void Writer<MessageT>::OnChannelChange(const proto::ChangeMsg& change_msg) {
  if (change_msg.role_type() != proto::RoleType::ROLE_READER) {
    return;
  }

  auto& reader_attr = change_msg.role_attr();
  if (reader_attr.channel_name() != this->role_attr_.channel_name()) {
    return;
  }

  auto operate_type = change_msg.operate_type();
  if (operate_type == proto::OperateType::OPT_JOIN) {
    transmitter_->Enable(reader_attr);
  } else {
    transmitter_->Disable(reader_attr);
  }
}
```

# Reader/ReaderBase

![Inheritance graph](assets/classapollo_1_1cyber_1_1ReaderBase__inherit__graph.png)

## ReaderBase

子类需要实现

```cpp
virtual bool Init() = 0;
virtual void Shutdown() = 0;
virtual void ClearData() = 0;
virtual void Observe() = 0;
virtual bool Empty() const = 0;
virtual bool HasReceived() const = 0;
virtual double GetDelaySec() const = 0;
virtual uint32_t PendingQueueSize() const = 0;
```

## Reader

### 成员变量

```cpp
using BlockerPtr = std::unique_ptr<blocker::Blocker<MessageT>>;
using ReceiverPtr = std::shared_ptr<transport::Receiver<MessageT>>;
using ChangeConnection =
    typename service_discovery::Manager::ChangeConnection;
using Iterator =
    typename std::list<std::shared_ptr<MessageT>>::const_iterator;

CallbackFunc<MessageT> reader_func_;
ReceiverPtr receiver_ = nullptr;
std::string croutine_name_;

BlockerPtr blocker_ = nullptr;

ChangeConnection change_conn_;
service_discovery::ChannelManagerPtr channel_manager_ = nullptr;
```

### * Init

```cpp
// 消息入队列，如果设置了reader_func_，一般都会设置，执行reader_func_，即 最上层的listener.cc获取数据后怎么办
std::function<void(const std::shared_ptr<MessageT>&)> func;
if (reader_func_ != nullptr) {
  func = [this](const std::shared_ptr<MessageT>& msg) {
    this->Enqueue(msg);
    this->reader_func_(msg);
  };
} else {
  func = [this](const std::shared_ptr<MessageT>& msg) { this->Enqueue(msg); };
}
```

上面设置的func执行时，即消息到来时。怎么执行的? **DataVisitor**观测数据?

```cpp
// 获取调度器，执行一个和DataVisitor相关的协程
auto sched = scheduler::Instance();
croutine_name_ = role_attr_.node_name() + "_" + role_attr_.channel_name();
auto dv = std::make_shared<data::DataVisitor<MessageT>>(
    role_attr_.channel_id(), pending_queue_size_);
// Using factory to wrap templates.
croutine::RoutineFactory factory =
    croutine::CreateRoutineFactory<MessageT>(std::move(func), dv);
if (!sched->CreateTask(factory, croutine_name_)) {
  AERROR << "Create Task Failed!";
  init_.exchange(false);
  return false;
}
```



```cpp
//获取接收器，相同话题的接收器Receiver其实只有一个，会派发消息给多个Reader
receiver_ = ReceiverManager<MessageT>::Instance()->GetReceiver(role_attr_);
this->role_attr_.set_id(receiver_->id().HashValue());
channel_manager_ =
    service_discovery::TopologyManager::Instance()->channel_manager();
JoinTheTopology
```

### JoinTheTopology

```cpp
template <typename MessageT>
void Reader<MessageT>::JoinTheTopology() {
  // add listener //  监听channel_manager_中的变化信息(粉丝的变化)，回调到自己的OnChannelChange
  change_conn_ = channel_manager_->AddChangeListener(std::bind(
      &Reader<MessageT>::OnChannelChange, this, std::placeholders::_1));

  // get peer writers 读写者的创建并没有先后顺序，自己创建之前就发布信息的写者在本阶段加入。
  const std::string& channel_name = this->role_attr_.channel_name();
  std::vector<proto::RoleAttributes> writers;
  channel_manager_->GetWritersOfChannel(channel_name, &writers);
  for (auto& writer : writers) {
    receiver_->Enable(writer);//enable
  }
    
  //作为订阅者加入
  channel_manager_->Join(this->role_attr_, proto::RoleType::ROLE_READER,
                         message::HasSerializer<MessageT>::value);
}
```





## ReceiverManager

单例类，根据节点的属性可以访问到对应的Receiver

```cpp
std::unordered_map<std::string,
                   typename std::shared_ptr<transport::Receiver<MessageT>>>
    receiver_map_;
std::mutex receiver_map_mutex_;

auto GetReceiver(const proto::RoleAttributes& role_attr) ->
    typename std::shared_ptr<transport::Receiver<MessageT>>;
```

### GetReceiver

//因为一个通道的多读取器会多次写入数据缓存，

//所以读取数据缓存时，我们使用map为每个通道保留一个实例 

//同一个Node不能有相同的Reader，但是不同的Node可以有相同的Reader。一个Node写，多个Node读。

```cpp
if (receiver_map_.count(channel_name) == 0) {
  receiver_map_[channel_name] =
      transport::Transport::Instance()->CreateReceiver<MessageT>(
          role_attr, [](const std::shared_ptr<MessageT>& msg,
                        const transport::MessageInfo& msg_info,
                        const proto::RoleAttributes& reader_attr) {
            (void)msg_info;
            (void)reader_attr;
            PerfEventCache::Instance()->AddTransportEvent(//1 TRANS_TO 事件
                TransPerf::TRANS_TO, reader_attr.channel_id(),
                msg_info.seq_num());
            data::DataDispatcher<MessageT>::Instance()->Dispatch(//2 派发数据?
                reader_attr.channel_id(), msg);
            PerfEventCache::Instance()->AddTransportEvent( //3 WRITE_NOTIFY 事件
                TransPerf::WRITE_NOTIFY, reader_attr.channel_id(),
                msg_info.seq_num());
          });
}
```

