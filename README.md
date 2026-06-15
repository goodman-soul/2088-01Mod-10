# 三进程乒乓游戏（无名管道 + 信号同步）

本程序创建 3 个进程：父进程、子进程1、子进程2。
使用 **一个无名管道** 在三个进程之间传递整数，并通过信号做严格轮转同步。

- 父进程先发送 `0` 给子进程1
- 子进程1加1后回传给父进程
- 父进程加1后发送给子进程2
- 子进程2加1后回传给父进程
- 循环往复，直到值 `> 最大常量`

打印规则：
- 每个进程打印的都是“自己加1后”的值
- 使用 `fprintf` 打印
- 每次打印后都 `fflush(stdout)`

## 本地运行

```bash
make
./pingpong
```

## Docker Compose 一键运行

```bash
docker compose build
docker compose run --rm pingpong
```

启动后在终端输入最大常量（例如 `10`）。

停止并清理：

```bash
docker compose down
```
