# 文件变化监听

`<culib/fs_watcher.h>` 提供 `wavvix::fs::watcher` 和 `wavvix::fs::poller`。监听本机目录、希望尽快收到变化时用 `watcher`；监听网络挂载，或需要定期核对目录最终状态时用 `poller`。完整可运行代码见 [watcher 示例](../examples/fs_watcher.cpp)和 [poller 示例](../examples/fs_poller.cpp)。

```cpp
wavvix::scheduler_scope runtime;
wavvix::fs::watcher changes;
auto watched = changes.watch(directory, wavvix::fs::recursive::yes);
if (watched.is_err()) return 1;

std::vector<wavvix::fs::change_event> events;
auto received = changes.wait(std::chrono::seconds(1), events);
if (received.is_err()) return 1;
for (const auto& event : events) {
    // event.watched_directory 是注册的根目录，event.path 是相对路径。
}
```

`watch()` 只接受已存在的目录，不补发注册前的历史事件。`unwatch()` 取消一个已注册的根目录；未注册时返回 `not_found`。`wait()` 先清空输出，收到事件时返回数量，超时时返回 0；负时长表示一直等。没有已注册目录时返回 `invalid_argument`。同一个监听器的操作不能并发，析构前必须让等待返回。事件中的两个路径都是指向监听器内部存储的 `string_view`，在下一次 `wait()` 或监听器析构前有效；需要长期保存就复制为 `std::string`。

`change::created`、`modified`、`removed` 是能够确定的变化；`changed` 表示平台通知无法可靠分类。macOS 的 FSEvents 会合并通知，普通通知统一报告 `changed`。重命名可能表现为移除和创建。`overflow` 表示有事件丢失，调用方应重扫目录。通知不保证每次写入恰好一个事件，也可能在调用方看到事件前合并多次变化。

`watcher` 使用 Linux inotify、macOS FSEvents 或 Windows `ReadDirectoryChangesW`。这些机制不保证发现网络挂载上其他机器所做的更改。`poller` 每个周期重扫目录，代价随目录规模增加，也可能漏掉两个扫描周期之间创建又删除的文件。需要更强的最终一致性时，可以用两个线程式 `coroutine` 分别等 `watcher` 和 `poller`，并按路径合并结果。
