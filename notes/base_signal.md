signal.h

# Signal

```cpp
SlotList slots_; // 多个回调函数
```

最重要的函数，传入cb，生成slot，返回connect(slot, signal)。提供给传入slot的一方，来disconnect/connect

```cpp
ConnectionType Connect(const Callback& cb) 
{
  auto slot = std::make_shared<Slot<Args...>>(cb);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.emplace_back(slot);
  }
  return ConnectionType(slot, this);
}
```

挨个执行

```cpp
void operator()(Args... args) {
  SlotList local;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
      local.emplace_back(slot);
    }
  }
  if (!local.empty()) {
    for (auto& slot : local) {
      (*slot)(args...);
    }
  }
  ClearDisconnectedSlots();
}
```



# Connection

记录 "slot 和其对应的 signal "的数据结构。装着两个指针，方便互相操作。

slot方并不会直接拿到slot，而是拿到connect，通过操作connect去操作slot

```cpp
  SlotPtr slot_;
  SignalPtr signal_;
```



# Slot

回调函数的载体，就是个回调函数



# 总结

- 信号槽机制的实现

- connect和disconnect都会从signal中添加/清除slot，并没有一个enable的概念