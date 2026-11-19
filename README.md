# magicBash

magicBash 基于 GNU Bash 5.3 和 aibash。除了常规 Bash 命令，你还可以用自然语言列目录、查看文件和执行命令。

## 编译

在 Debian 或 Ubuntu 上安装依赖并编译：

```bash
sudo apt install git build-essential bison libcurl4-openssl-dev libreadline-dev
git clone https://github.com/XirroZheng/magicBash.git
cd magicBash
./configure
make -j"$(nproc)"
```

## 配置与启动

在 `~/.bashllmrc` 中填写 OpenAI 兼容接口的地址、模型名和密钥：

```ini
[model]
url = https://api.example.com/v1/chat/completions
model = 你的模型名
key = 你的API密钥
```

在项目目录运行 `./aibash`。提示符会以 `(aibash)` 开头。

## 示例

```bash
printf 'hello magicBash\n' > demo.txt
llm 打印 demo.txt 的内容
```

终端输出 `hello magicBash`。

## 模型工具沙箱

Linux 上安装 Bubblewrap、Python 3.9+ 和 Git，在 `~/.bashllmrc` 的 `[settings]` 中把 `sandbox_root` 设为项目的绝对路径，再从该项目内启动 aibash。以 root 启动时需要预先创建非登录账号 `magicbash-sandbox`。

每轮模型工具调用共享一份项目副本，工具进程的网络独立隔离。aibash 预览普通文件和目录的变化，并在确认后写回项目。未确认的修改随本轮工作区清理。

[上游 aibash](https://github.com/jstormes/aibash) · [原 README](README.upstream.md) · [许可证](COPYING)
