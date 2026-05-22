
去open ai api platform
去chat
开两个chat

# 第一个是ai 解题
dev prompt :  paste 我们的第一个中文prompt
user prompt : 随意 optional
model : gpt5.5 or 更latest 的
thinking : high
summary : null
if 你的学科很难 有专业性 壁垒, 可以开file search 自己建一个rag,放你的课本 资源

# 第二个ai for seperate block 
dev prompt :  paste 我们的第二个英文prompt
user prompt : 随意 optional
model : gpt5.4 or 5.3
thinking : medium
summary : null
variables : answer_full

做好后copy id 和 version, server 直接调用这两个 id 的prompt 而不是自己 api 调用model 然后每次都上传dev prompt, 特别麻烦 效率低, 而且open ai api platform 可以realtime test 各种Prompt

# prompt 
我已经测试了很多遍, 暂时这两个prompt 都能很好的work 了, 尤其第二个, 现在已经足够聪明且稳定, 可以把第一轮ai 解题解答 根据 各种latex, 中英文 等 切割成各种大大小小不同高度 block 并用json 分割开来, 相当于是让后续服务器知道计算机每一行 每个block 要显示什么, 布局计算机的 解答的上下左右移动的方块和显示. 然后我们server 就会获取这个第二次的ai output 然后拿去进行latex 转成像素显示, 然后我们在把这些一个一个像素化的latex 数学公式, 中英文解答 从像素 根据block 长度宽度 进行1bitmap 化, 1bitmap 就是让esp32 知道 一个screen 里面每个点是要黑色还是白色, 就像给esp32 0001110101 这样的显示数据 but 1bitmap更聪明 更压缩一点, so esp 就能直接获取server 的解答 然后显示