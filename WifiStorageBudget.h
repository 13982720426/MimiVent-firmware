#pragma once
#include <stddef.h>
namespace WifiStorageBudget {
// 当前 4 KiB NVS 页有 126 entry；真机测得 OTA 两槽 48、单次重写 24，绑定两槽约 25。
// 保留 GC 空页与 9 entry 余量，不能把统计中的全部 free entry 当可用空间。
constexpr size_t required(bool tableExists){return 126+48+24+9+(tableExists?0:25);}
constexpr bool allowsBinding(size_t freeEntries,bool tableExists){return freeEntries>=required(tableExists);}
}
