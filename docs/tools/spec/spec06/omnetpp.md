以太网仿真算法

```c
  19.36%  471.omnetpp  471.omnetpp        [.] cMessageHeap::shiftup(int)                                                                                             ◆
   7.22%  471.omnetpp  471.omnetpp        [.] _int_malloc                                                                                                            ▒
   5.15%  471.omnetpp  471.omnetpp        [.] cMessageHeap::insert(cMessage*)                                                                                        ▒
   4.44%  471.omnetpp  471.omnetpp        [.] cObject::setOwner(cObject*)       
```



```c
 h = new cMessage *[size+1];

void cMessageHeap::shiftup(int from)
{
    int i = from;
    while ((j=2*i) <= n)  // n是堆中元素总数
    {
        // 1. 选择左右子节点中较小的一个
        if (j<n && (*h[j] > *h[j+1]))   // 如果右子节点存在且比左子节点小
            j++;                         // j指向较小的子节点
            
        // 2. 如果当前节点大于子节点中的较小者，则交换
        if (*h[i] > *h[j])  
        {
            // 交换节点
            temp = h[j];
            (h[j]=h[i])->heapindex=j;
            (h[i]=temp)->heapindex=i;
            i = j;  // 继续向下检查
        }
        else
            break;  // 堆性质已满足，退出
    }
}
```

h是指针数组

<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">关键点说明：</font>

+ **<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">数据结构</font>**<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">:</font>
+ <font style="background-color:rgb(243, 243, 243);">h</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">是一个指针</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">数组，存</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">储</font><font style="background-color:rgb(243, 243, 243);">cMessage</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">对</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">象的指针</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">使用</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">1-based索引</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">（h[0]不使</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">用）</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">对于节点</font><font style="background-color:rgb(243, 243, 243);">i</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">，其左子节点</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">为</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">2i</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">，右子节点</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">为</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">2i+</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">1</font>
+ **<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">比</font>****<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">较操作</font>**<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">:</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">通过重</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">载的</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">operator</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">></font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">进行消</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">息比较</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">比较优</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">先级：到</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">达时间(arr</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">ivalTime) > 优先级</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">(priority) > 插</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">入顺序</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">(insertOrder)</font>
+ **<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">堆的维护</font>**<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">:</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">函数名</font><font style="background-color:rgb(243, 243, 243);">shiftup</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">实</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">际上执</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">行的是</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">"下沉"操作</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">（有些</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">地方也叫s</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">ift-down或heapify</font><font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">）</font>
+ <font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">从给定节点开始，不断与子节点比较并交换，直到满足堆的性质</font>

<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);"></font>

<font style="color:rgb(0, 0, 0);background-color:rgb(243, 243, 243);">应该是指针预取器有效，在好的预取器上效果不错，目前的MPKI=7，已经不错了</font>

