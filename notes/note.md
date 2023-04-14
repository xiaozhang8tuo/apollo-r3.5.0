Node 通过 readers_mutex_  维护 std::map<std::string, std::shared_ptr<ReaderBase>> readers_;

channel/chatter channel_name 即话题



一个Node可以订阅(reader)多个话题(channel)