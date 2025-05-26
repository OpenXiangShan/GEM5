x86 gcc 编译，有大约2.7% cmov 指令，并不算是热点

```bash
  22.29%  458.sjeng  458.sjeng         [.] eval                                                                                                                                                                              ◆
  11.24%  458.sjeng  458.sjeng         [.] gen                                                                                                                                                                               ▒
   9.56%  458.sjeng  458.sjeng         [.] search    

在search函数中有一小块for循环中的小的if指令块
  for (i = 0; i < num_moves; i++) {
    if (move_ordering[i] > best) {
      *marker = i;
      best = move_ordering[i];
    }
  }
```





