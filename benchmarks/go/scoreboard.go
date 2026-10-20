// culib 记分板的 Go 一侧：项目、单位、规模和做法都与 benchmarks/scoreboard.cpp 一一对应。
//
//	scoreboard          列出全部项目
//	scoreboard NAME     跑一项，打印「NAME 数值 单位」；这个平台上量不了的打印 "-"
//
// GOMAXPROCS 对应 culib 的 WAVVIX_MAXPROCS。
package main

import (
	"bufio"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	spawnCount      = 200000
	yieldCount      = 1000000
	pingpongRounds  = 200000
	crowd           = 10000 // convoy、release、parked_memory
	lockOps         = 10000000
	contenders      = 8
	contendedOps    = 100000 // 每根 goroutine
	channelItems    = 1000000
	channelCapacity = 1024
	channelSides    = 4 // chan_mpmc 的发送方和接收方各几根
	sleepers        = 10000
	shortSleeps     = 101
	echoConnections = 64
	echoRounds      = 2000
	connectCount    = 1000
	udpRounds       = 100000
	messageSize     = 64
	allocOps        = 10000000
	allocWorkers    = 8
	allocOpsEach    = allocOps / allocWorkers
	ringSize        = 1024
	fileBytes       = 64 << 20
	fileChunk       = 64 << 10
	walkDirs        = 100
	walkFiles       = 100
	removeDirs      = 20
	removeFiles     = 100
	logLines        = 200000
	sampleCount     = 1024
	textOps         = 8388608 // sampleCount 的整数倍
	splitFields     = 32
	splitOps        = 1000000
)

// ---- 共用

func check(ok bool, what string) {
	if !ok {
		fmt.Fprintf(os.Stderr, "scoreboard: %s\n", what)
		os.Exit(1)
	}
}

func must(err error, what string) {
	if err != nil {
		fmt.Fprintf(os.Stderr, "scoreboard: %s: %v\n", what, err)
		os.Exit(1)
	}
}

func nanos(elapsed time.Duration) float64 {
	return float64(elapsed.Nanoseconds())
}

func millis(elapsed time.Duration) float64 {
	return nanos(elapsed) / 1e6
}

// 每秒多少 MB（10^6 字节）。
func megabytesPerSecond(bytes int64, elapsed time.Duration) float64 {
	return float64(bytes) / 1e6 / elapsed.Seconds()
}

func nextRandom(state *uint64) uint64 {
	*state ^= *state << 13
	*state ^= *state >> 7
	*state ^= *state << 17
	return *state
}

func pauseBriefly() {
	time.Sleep(200 * time.Microsecond)
}

// ---- 调度

// 起 spawnCount 根什么都不做的 goroutine，等它们全部结束；每根多少纳秒。
func spawn() float64 {
	start := time.Now()
	var group sync.WaitGroup
	for i := 0; i < spawnCount; i++ {
		group.Add(1)
		go func() {
			group.Done()
		}()
	}
	group.Wait()
	return nanos(time.Since(start)) / spawnCount
}

// 一根 goroutine 让出 yieldCount 次；每次多少纳秒。
func yields() float64 {
	start := time.Now()
	for i := 0; i < yieldCount; i++ {
		runtime.Gosched()
	}
	return nanos(time.Since(start)) / yieldCount
}

// 两根 goroutine 靠一把锁和一个条件变量轮流；每个来回多少纳秒。
func pingpongCond() float64 {
	var lock sync.Mutex
	changed := sync.NewCond(&lock)
	turn := 0
	var other sync.WaitGroup
	other.Add(1)
	go func() {
		defer other.Done()
		for r := 0; r < pingpongRounds; r++ {
			lock.Lock()
			for turn != 1 {
				changed.Wait()
			}
			turn = 0
			changed.Signal()
			lock.Unlock()
		}
	}()
	start := time.Now()
	for r := 0; r < pingpongRounds; r++ {
		lock.Lock()
		turn = 1
		changed.Signal()
		for turn != 0 {
			changed.Wait()
		}
		lock.Unlock()
	}
	elapsed := time.Since(start)
	other.Wait()
	return nanos(elapsed) / pingpongRounds
}

// 两根 goroutine 经两个容量为 1 的通道传一个数；每个来回多少纳秒。
func pingpongChan() float64 {
	there := make(chan int, 1)
	back := make(chan int, 1)
	var other sync.WaitGroup
	other.Add(1)
	go func() {
		defer other.Done()
		for value := range there {
			back <- value + 1
		}
	}()
	start := time.Now()
	value := 0
	for r := 0; r < pingpongRounds; r++ {
		there <- value
		value = <-back
	}
	elapsed := time.Since(start)
	close(there)
	other.Wait()
	check(value == pingpongRounds, "pingpong_chan: lost a round")
	return nanos(elapsed) / pingpongRounds
}

// crowd 根 goroutine 排在一把锁上，放开以后到全部拿过一次锁的毫秒数。
func convoy() float64 {
	var lock sync.Mutex
	var waiting atomic.Int64
	var group sync.WaitGroup
	lock.Lock()
	for i := 0; i < crowd; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			waiting.Add(1)
			lock.Lock()
			lock.Unlock()
		}()
	}
	for waiting.Load() < crowd {
		pauseBriefly()
	}
	pauseBriefly()
	start := time.Now()
	lock.Unlock()
	group.Wait()
	return millis(time.Since(start))
}

// crowd 根 goroutine 等在一个条件变量上。全部等着时调一次 ready，然后 Broadcast，返回放行到全部结束的时长。
func releaseCrowd(ready func()) time.Duration {
	var gate sync.Mutex
	opened := sync.NewCond(&gate)
	open := false
	var waiting atomic.Int64
	var group sync.WaitGroup
	for i := 0; i < crowd; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			gate.Lock()
			waiting.Add(1)
			for !open {
				opened.Wait()
			}
			gate.Unlock()
		}()
	}
	for waiting.Load() < crowd {
		pauseBriefly()
	}
	ready()
	start := time.Now()
	gate.Lock()
	open = true
	opened.Broadcast()
	gate.Unlock()
	group.Wait()
	return time.Since(start)
}

func release() float64 {
	return millis(releaseCrowd(func() {}))
}

// 进程的常驻内存，KiB。
func residentKiB() int64 {
	status, err := os.Open("/proc/self/status")
	if err != nil {
		return 0
	}
	defer status.Close()
	lines := bufio.NewScanner(status)
	for lines.Scan() {
		fields := strings.Fields(lines.Text())
		if len(fields) >= 2 && fields[0] == "VmRSS:" {
			value, _ := strconv.ParseInt(fields[1], 10, 64)
			return value
		}
	}
	return 0
}

// crowd 根 goroutine 挂起等着时，每根多占多少 KiB 常驻内存。只在 Linux 上量。
func parkedMemory() float64 {
	if runtime.GOOS != "linux" {
		return -1 // 量不了
	}
	before := residentKiB()
	during := before
	releaseCrowd(func() { during = residentKiB() })
	return float64(during-before) / crowd
}

// ---- 同步

// 一根 goroutine 拿放一把没人抢的锁；每次多少纳秒。
func mutexUncontended() float64 {
	var lock sync.Mutex
	counter := 0
	start := time.Now()
	for i := 0; i < lockOps; i++ {
		lock.Lock()
		counter++
		lock.Unlock()
	}
	elapsed := time.Since(start)
	check(counter == lockOps, "mutex_uncontended: wrong count")
	return nanos(elapsed) / lockOps
}

// contenders 根 goroutine 各拿放 contendedOps 次同一把锁；总时长除以总次数，纳秒。
func mutexContended() float64 {
	var lock sync.Mutex
	counter := 0
	start := time.Now()
	var group sync.WaitGroup
	for c := 0; c < contenders; c++ {
		group.Add(1)
		go func() {
			defer group.Done()
			for i := 0; i < contendedOps; i++ {
				lock.Lock()
				counter++
				lock.Unlock()
			}
		}()
	}
	group.Wait()
	elapsed := time.Since(start)
	check(counter == contenders*contendedOps, "mutex_contended: wrong count")
	return nanos(elapsed) / (contenders * contendedOps)
}

// 一根 goroutine 往容量 channelCapacity 的通道里发 channelItems 个数，另一根收；每个多少纳秒。
func chanSPSC() float64 {
	items := make(chan int, channelCapacity)
	sum := 0
	start := time.Now()
	go func() {
		for i := 0; i < channelItems; i++ {
			items <- i
		}
		close(items)
	}()
	for value := range items {
		sum += value
	}
	elapsed := time.Since(start)
	check(sum == channelItems*(channelItems-1)/2, "chan_spsc: wrong sum")
	return nanos(elapsed) / channelItems
}

// channelSides 根 goroutine 发、channelSides 根收，一共 channelItems 个数；每个多少纳秒。
func chanMPMC() float64 {
	items := make(chan int, channelCapacity)
	var sum atomic.Int64
	start := time.Now()
	var receivers sync.WaitGroup
	for c := 0; c < channelSides; c++ {
		receivers.Add(1)
		go func() {
			defer receivers.Done()
			local := int64(0)
			for value := range items {
				local += int64(value)
			}
			sum.Add(local)
		}()
	}
	var senders sync.WaitGroup
	for p := 0; p < channelSides; p++ {
		senders.Add(1)
		go func(p int) {
			defer senders.Done()
			for i := p; i < channelItems; i += channelSides {
				items <- i
			}
		}(p)
	}
	senders.Wait()
	close(items)
	receivers.Wait()
	elapsed := time.Since(start)
	check(sum.Load() == int64(channelItems)*(channelItems-1)/2, "chan_mpmc: wrong sum")
	return nanos(elapsed) / channelItems
}

// ---- 定时器

// sleepers 根 goroutine 各睡 10 毫秒，从起第一根到全部醒来的毫秒数，理想是 10 出头。
func sleepMany() float64 {
	start := time.Now()
	var group sync.WaitGroup
	for i := 0; i < sleepers; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			time.Sleep(10 * time.Millisecond)
		}()
	}
	group.Wait()
	return millis(time.Since(start))
}

// 一根 goroutine 睡 shortSleeps 次 1 毫秒，实际睡了多少微秒的中位数。
func sleep1ms() float64 {
	slept := make([]float64, 0, shortSleeps)
	for i := 0; i < shortSleeps; i++ {
		start := time.Now()
		time.Sleep(time.Millisecond)
		slept = append(slept, nanos(time.Since(start))/1e3)
	}
	sort.Float64s(slept)
	return slept[len(slept)/2]
}

// ---- 网络

// 读到多少写回多少，对端关闭就结束。
func echoBack(peer net.Conn) {
	defer peer.Close()
	buffer := make([]byte, 4096)
	for {
		n, err := peer.Read(buffer)
		if err != nil || n == 0 {
			return
		}
		if _, err := peer.Write(buffer[:n]); err != nil {
			return
		}
	}
}

// 回环上的回显服务端：接受 connections 个连接，每个连接一根 goroutine，都记在 group 里。
func startEchoServer(connections int, group *sync.WaitGroup) net.Listener {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	must(err, "listen")
	group.Add(1)
	go func() {
		defer group.Done()
		for i := 0; i < connections; i++ {
			peer, err := listener.Accept()
			must(err, "accept")
			group.Add(1)
			go func() {
				defer group.Done()
				echoBack(peer)
			}()
		}
	}()
	return listener
}

// echoConnections 个连接同时往返 64 字节各 echoRounds 次；总时长除以总来回数，纳秒。
func tcpEcho() float64 {
	var servers sync.WaitGroup
	listener := startEchoServer(echoConnections, &servers)
	address := listener.Addr().String()
	start := time.Now()
	var clients sync.WaitGroup
	for c := 0; c < echoConnections; c++ {
		clients.Add(1)
		go func() {
			defer clients.Done()
			client, err := net.Dial("tcp", address)
			must(err, "dial")
			defer client.Close()
			message := make([]byte, messageSize)
			for r := 0; r < echoRounds; r++ {
				message[0] = byte(r)
				_, err := client.Write(message)
				must(err, "write")
				_, err = io.ReadFull(client, message)
				must(err, "read")
			}
		}()
	}
	clients.Wait()
	elapsed := time.Since(start)
	servers.Wait()
	listener.Close()
	return nanos(elapsed) / (echoConnections * echoRounds)
}

// 依次建 connectCount 个连接，每个往返 64 字节后关闭；每个连接多少微秒。
func tcpConnect() float64 {
	var servers sync.WaitGroup
	listener := startEchoServer(connectCount, &servers)
	address := listener.Addr().String()
	message := make([]byte, messageSize)
	start := time.Now()
	for i := 0; i < connectCount; i++ {
		client, err := net.Dial("tcp", address)
		must(err, "dial")
		_, err = client.Write(message)
		must(err, "write")
		_, err = io.ReadFull(client, message)
		must(err, "read")
		client.Close()
	}
	elapsed := time.Since(start)
	servers.Wait()
	listener.Close()
	return nanos(elapsed) / 1e3 / connectCount
}

// 两个 UDP 套接字来回传 64 字节的报文 udpRounds 次；每个来回多少纳秒。
func udpEcho() float64 {
	loopback := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)}
	server, err := net.ListenUDP("udp", loopback)
	must(err, "listen udp")
	defer server.Close()
	client, err := net.ListenUDP("udp", loopback)
	must(err, "listen udp")
	defer client.Close()
	serverAt := server.LocalAddr().(*net.UDPAddr).AddrPort()

	var echo sync.WaitGroup
	echo.Add(1)
	go func() {
		defer echo.Done()
		buffer := make([]byte, 2048)
		for r := 0; r < udpRounds; r++ {
			n, from, err := server.ReadFromUDPAddrPort(buffer)
			must(err, "recv")
			_, err = server.WriteToUDPAddrPort(buffer[:n], from)
			must(err, "send")
		}
	}()
	message := make([]byte, messageSize)
	start := time.Now()
	for r := 0; r < udpRounds; r++ {
		_, err := client.WriteToUDPAddrPort(message, serverAt)
		must(err, "send")
		_, _, err = client.ReadFromUDPAddrPort(message)
		must(err, "recv")
	}
	elapsed := time.Since(start)
	echo.Wait()
	return nanos(elapsed) / udpRounds
}

// ---- 内存

// 一圈 ringSize 个格子，每次往一格里放一块 16 到 271 字节的新块，旧块交给垃圾回收，做 ops 次。
func churn(ops int, seed uint64) time.Duration {
	ring := make([][]byte, ringSize)
	state := seed
	start := time.Now()
	for i := 0; i < ops; i++ {
		block := make([]byte, 16+nextRandom(&state)&255)
		block[0] = 1
		ring[i&(ringSize-1)] = block
	}
	return time.Since(start)
}

// 一根 goroutine 反复分配小块；每次多少纳秒。
func allocSmall() float64 {
	return nanos(churn(allocOps, 88172645463325252)) / allocOps
}

// allocWorkers 根 goroutine 同时做上面的事，各 allocOpsEach 次；总时长除以每根的次数，纳秒。
func allocParallel() float64 {
	start := time.Now()
	var group sync.WaitGroup
	for w := 0; w < allocWorkers; w++ {
		group.Add(1)
		go func(w int) {
			defer group.Done()
			churn(allocOpsEach, 88172645463325252+uint64(w))
		}(w)
	}
	group.Wait()
	return nanos(time.Since(start)) / allocOpsEach
}

// ---- 文件

// 临时目录下新建的空目录，remove 时连同里面的东西删掉。
type scratch struct {
	root string
}

func newScratch() scratch {
	root, err := os.MkdirTemp("", "culib_scoreboard_")
	must(err, "mkdir temp")
	return scratch{root}
}

func (s scratch) at(name string) string {
	return filepath.Join(s.root, name)
}

func (s scratch) remove() {
	if err := os.RemoveAll(s.root); err != nil {
		fmt.Fprintf(os.Stderr, "scoreboard: cannot remove %s\n", s.root)
	}
}

// 写 bytes 字节，每次 fileChunk。
func writeFile(target string, bytes int) {
	chunk := make([]byte, fileChunk)
	for i := range chunk {
		chunk[i] = 'x'
	}
	out, err := os.Create(target)
	must(err, "create")
	for written := 0; written < bytes; written += len(chunk) {
		chunk[0] = byte(written >> 16)
		_, err := out.Write(chunk)
		must(err, "write")
	}
	must(out.Close(), "close")
}

// 写 fileBytes 字节，每次 64 KiB；MB/s。
func fileWrite() float64 {
	s := newScratch()
	defer s.remove()
	start := time.Now()
	writeFile(s.at("data"), fileBytes)
	return megabytesPerSecond(fileBytes, time.Since(start))
}

// 读回刚写的文件，每次 64 KiB；MB/s。
func fileRead() float64 {
	s := newScratch()
	defer s.remove()
	data := s.at("data")
	writeFile(data, fileBytes)
	chunk := make([]byte, fileChunk)
	total := int64(0)
	start := time.Now()
	in, err := os.Open(data)
	must(err, "open")
	for {
		n, err := in.Read(chunk)
		total += int64(n)
		if err == io.EOF {
			break
		}
		must(err, "read")
	}
	elapsed := time.Since(start)
	in.Close()
	check(total == fileBytes, "file_read: wrong size")
	return megabytesPerSecond(total, elapsed)
}

// 复制刚写的文件（目标已存在时报错）；MB/s。
func copyFile() float64 {
	s := newScratch()
	defer s.remove()
	writeFile(s.at("from"), fileBytes)
	start := time.Now()
	from, err := os.Open(s.at("from"))
	must(err, "open")
	to, err := os.OpenFile(s.at("to"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	must(err, "create")
	_, err = io.Copy(to, from)
	must(err, "copy")
	must(to.Close(), "close")
	from.Close()
	return megabytesPerSecond(fileBytes, time.Since(start))
}

// dirs 个子目录，各 files 个空文件。
func buildTree(root string, dirs, files int) {
	for d := 0; d < dirs; d++ {
		directory := filepath.Join(root, "d"+strconv.Itoa(d))
		must(os.Mkdir(directory, 0o755), "mkdir")
		for f := 0; f < files; f++ {
			file, err := os.Create(filepath.Join(directory, "f"+strconv.Itoa(f)))
			must(err, "create")
			file.Close()
		}
	}
}

// 递归遍历 walkDirs 个目录、各 walkFiles 个文件的树，每一项都分清是不是目录；每项多少纳秒。
func walk() float64 {
	s := newScratch()
	defer s.remove()
	buildTree(s.root, walkDirs, walkFiles)
	entries := 0
	directories := 0
	start := time.Now()
	err := filepath.WalkDir(s.root, func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == s.root {
			return nil
		}
		entries++
		if entry.IsDir() {
			directories++
		}
		return nil
	})
	elapsed := time.Since(start)
	must(err, "walk")
	check(entries == walkDirs*(walkFiles+1) && directories == walkDirs, "walk: wrong count")
	return nanos(elapsed) / float64(entries)
}

// 删掉 removeDirs 个目录、各 removeFiles 个文件的树（连根一共这么多项）；每项多少纳秒。
func removeTree() float64 {
	s := newScratch()
	defer s.remove()
	tree := s.at("tree")
	must(os.Mkdir(tree, 0o755), "mkdir")
	buildTree(tree, removeDirs, removeFiles)
	entries := removeDirs*(removeFiles+1) + 1
	start := time.Now()
	must(os.RemoveAll(tree), "remove all")
	return nanos(time.Since(start)) / float64(entries)
}

// ---- 日志

// 往文件里写 logLines 行带时间和级别的日志，每行一次写；每行多少纳秒。
func logFile() float64 {
	s := newScratch()
	defer s.remove()
	file, err := os.OpenFile(s.at("app.log"), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	must(err, "open log")
	logger := log.New(file, "", log.LstdFlags|log.Lmicroseconds)
	start := time.Now()
	for i := 0; i < logLines; i++ {
		logger.Printf("INFO request %d done in %d us", i, i%1000)
	}
	elapsed := time.Since(start)
	file.Close()
	return nanos(elapsed) / logLines
}

// ---- 数字和文本
// 样本两边按同一个算法生成：1 到 18 位的整数，1 到 17 位尾数、十进制指数 -20 到 20 的小数。

func sampleIntegers() []int64 {
	values := make([]int64, 0, sampleCount)
	state := uint64(2463534242)
	for i := 0; i < sampleCount; i++ {
		r := nextRandom(&state)
		bound := uint64(1)
		for digits := 1 + r%18; digits > 0; digits-- {
			bound *= 10
		}
		magnitude := int64((r >> 8) % bound)
		if r&1 != 0 {
			magnitude = -magnitude
		}
		values = append(values, magnitude)
	}
	return values
}

func sampleDecimals() []string {
	texts := make([]string, 0, sampleCount)
	state := uint64(3141592653)
	for i := 0; i < sampleCount; i++ {
		r := nextRandom(&state)
		bound := uint64(1)
		for digits := 1 + r%17; digits > 0; digits-- {
			bound *= 10
		}
		exponent := int64((r>>4)%41) - 20
		texts = append(texts, strconv.FormatUint((r>>8)%bound, 10)+"e"+strconv.FormatInt(exponent, 10))
	}
	return texts
}

// 解析十进制整数；每次多少纳秒。
func parseInt() float64 {
	values := sampleIntegers()
	texts := make([]string, len(values))
	expected := uint64(0)
	for i, value := range values {
		texts[i] = strconv.FormatInt(value, 10)
		expected += uint64(value)
	}
	sum := uint64(0)
	start := time.Now()
	for i := 0; i < textOps; i++ {
		value, err := strconv.ParseInt(texts[i&(sampleCount-1)], 10, 64)
		if err != nil {
			panic(err)
		}
		sum += uint64(value)
	}
	elapsed := time.Since(start)
	check(sum == expected*(textOps/sampleCount), "parse_int: wrong sum")
	return nanos(elapsed) / textOps
}

// 解析小数；每次多少纳秒。
func parseDouble() float64 {
	texts := sampleDecimals()
	sum := 0.0
	start := time.Now()
	for i := 0; i < textOps; i++ {
		value, err := strconv.ParseFloat(texts[i&(sampleCount-1)], 64)
		if err != nil {
			panic(err)
		}
		sum += value
	}
	elapsed := time.Since(start)
	check(sum != 0, "parse_double: zero sum")
	return nanos(elapsed) / textOps
}

// 整数转成十进制文本；每次多少纳秒。
func formatInt() float64 {
	values := sampleIntegers()
	length := 0
	start := time.Now()
	for i := 0; i < textOps; i++ {
		length += len(strconv.FormatInt(values[i&(sampleCount-1)], 10))
	}
	elapsed := time.Since(start)
	check(length > 0, "format_int: empty")
	return nanos(elapsed) / textOps
}

// 小数转成能原样读回的最短文本；每次多少纳秒。
func formatDouble() float64 {
	texts := sampleDecimals()
	values := make([]float64, len(texts))
	for i, text := range texts {
		value, err := strconv.ParseFloat(text, 64)
		must(err, "parse")
		values[i] = value
	}
	length := 0
	start := time.Now()
	for i := 0; i < textOps; i++ {
		length += len(strconv.FormatFloat(values[i&(sampleCount-1)], 'g', -1, 64))
	}
	elapsed := time.Since(start)
	check(length > 0, "format_double: empty")
	return nanos(elapsed) / textOps
}

// 按逗号切开一行 splitFields 个字段；每次多少纳秒。
func splitLine() float64 {
	var line strings.Builder
	for f := 0; f < splitFields; f++ {
		if f > 0 {
			line.WriteByte(',')
		}
		line.WriteString("field" + strconv.Itoa(f))
	}
	text := line.String()
	parts := 0
	start := time.Now()
	for i := 0; i < splitOps; i++ {
		parts += len(strings.Split(text, ","))
	}
	elapsed := time.Since(start)
	check(parts == splitOps*splitFields, "split: wrong count")
	return nanos(elapsed) / splitOps
}

// ---- 项目表

type scoreboardCase struct {
	name string
	unit string
	run  func() float64
}

var cases = []scoreboardCase{
	{"spawn", "ns", spawn},
	{"yield", "ns", yields},
	{"pingpong_cond", "ns", pingpongCond},
	{"pingpong_chan", "ns", pingpongChan},
	{"convoy", "ms", convoy},
	{"release", "ms", release},
	{"parked_memory", "KiB", parkedMemory},
	{"mutex_uncontended", "ns", mutexUncontended},
	{"mutex_contended", "ns", mutexContended},
	{"chan_spsc", "ns", chanSPSC},
	{"chan_mpmc", "ns", chanMPMC},
	{"sleep_many", "ms", sleepMany},
	{"sleep_1ms", "us", sleep1ms},
	{"tcp_echo", "ns", tcpEcho},
	{"tcp_connect", "us", tcpConnect},
	{"udp_echo", "ns", udpEcho},
	{"alloc_small", "ns", allocSmall},
	{"alloc_parallel", "ns", allocParallel},
	{"file_write", "MB/s", fileWrite},
	{"file_read", "MB/s", fileRead},
	{"copy_file", "MB/s", copyFile},
	{"walk", "ns", walk},
	{"remove_tree", "ns", removeTree},
	{"log_file", "ns", logFile},
	{"parse_int", "ns", parseInt},
	{"parse_double", "ns", parseDouble},
	{"format_int", "ns", formatInt},
	{"format_double", "ns", formatDouble},
	{"split", "ns", splitLine},
}

func main() {
	if len(os.Args) < 2 {
		for _, entry := range cases {
			fmt.Printf("%s %s\n", entry.name, entry.unit)
		}
		return
	}
	for _, entry := range cases {
		if entry.name != os.Args[1] {
			continue
		}
		value := entry.run()
		if value < 0 { // 这个平台上量不了
			fmt.Printf("%s - %s\n", entry.name, entry.unit)
		} else {
			fmt.Printf("%s %.4g %s\n", entry.name, value, entry.unit)
		}
		return
	}
	fmt.Fprintf(os.Stderr, "scoreboard: no such case: %s\n", os.Args[1])
	os.Exit(2)
}
