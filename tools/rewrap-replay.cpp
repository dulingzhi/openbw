// rewrap-replay: 把双层容器的 Brood War 录像改写成 OpenBW 能直接读的分段格式。
//
// ## OpenBW 的读取方式（replay.h:41-69）
//
// 每次 get_bytes(buf, n) 读 [u32 crc32][u32 segments]，然后 segments 次
// [u32 len][len bytes]。每个段产出的字节数 = min(n - 已写, 8192)。段长等于该值
// 就原样拷贝，否则用 PKWare DCL 解压。一层的所有段产出拼起来必须正好 n 字节。
//
// ## 双层容器
//
// 实测两个来源结构相同：
//   * D:\Downloads\a.StarCraft1.16-jisu\maps\replays\LastReplay.rep（1.16 客户端自录）
//   * Huk Vs Boxer Game2.rep（2011 年比赛录像）
// 都是：
//
//   layer0 @0 : crc, segs=1, len=4, data="reRS"      <- OpenBW 读的 identifier
//   layer1 @16: crc, segs=1, len=N,  data=压缩块      <- 解压后正好 633 字节游戏信息头
//
// OpenBW 读完 "reRS" 后要 633 字节，于是按协议再读一层 [crc][segs]，而文件里
// 那里接着是 [len][压缩块]。它把 len(=183/197) 当成「本段只产出这么多字节」，
// 随后发现不足 633，报 output buffer too small 并抛出 → 未捕获即 abort。
//
// ## 本工具的做法
//
// 保持层结构不变，只把 layer1 的第一段改成「len == 解压后长度」的原样段，CRC
// 按解压后的字节重算。这样 OpenBW 读头部时命中原样拷贝分支。layer1 之后的数据
// 原样搬移，位置不变。
//
// 用法：rewrap-replay <in.rep> <out.rep>
#include "bwgame.h"
#include "replay.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint32_t read_u32(const std::vector<uint8_t>& d, size_t off) {
  uint32_t v = 0;
  std::memcpy(&v, d.data() + off, 4);
  return v;
}

void append_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i != 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: rewrap-replay <in.rep> <out.rep>\n");
    return 2;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::fseek(f, 0, SEEK_END);
  const long file_size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> in(static_cast<size_t>(file_size));
  if (std::fread(in.data(), 1, in.size(), f) != in.size()) {
    std::fclose(f);
    return 1;
  }
  std::fclose(f);

  if (in.size() < 32) {
    std::fprintf(stderr, "file too small\n");
    return 1;
  }
  // 只处理观测到的双层容器：第一层单段且内容为 "reRS"。
  const uint32_t l0_segs = read_u32(in, 4);
  const uint32_t l0_len = read_u32(in, 8);
  if (l0_segs != 1 || l0_len != 4 || std::memcmp(in.data() + 12, "reRS", 4) != 0) {
    std::fprintf(stderr,
      "not the observed container (segs=%u len=%u tag=%.4s); refusing to guess\n",
      l0_segs, l0_len, in.data() + 12);
    return 1;
  }
  const size_t l1_off = 12 + l0_len;
  const uint32_t l1_segs = read_u32(in, l1_off + 4);
  std::fprintf(stderr, "layer0: segs=1 len=4 tag=reRS\nlayer1@%zu: segs=%u\n", l1_off, l1_segs);
  if (l1_segs != 1) {
    std::fprintf(stderr, "layer1 has %u segments; this tool only rewrites the "
                 "single-segment header block it has verified\n", l1_segs);
    return 1;
  }
  const uint32_t l1_len = read_u32(in, l1_off + 8);
  const size_t l1_data = l1_off + 12;
  if (l1_data + l1_len > in.size()) {
    std::fprintf(stderr, "layer1: len %u runs past EOF\n", l1_len);
    return 1;
  }

  // 这一块是 633 字节游戏信息头（replay.h:110）。
  constexpr size_t kHeaderSize = 633;
  std::vector<uint8_t> header(kHeaderSize, 0);
  try {
    bwgame::data_loading::decompress(in.data() + l1_data, l1_len, header.data(), kHeaderSize);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "header decompress failed: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "header: %u stored -> %zu bytes, is_broodwar=%u frame_count=%u\n",
               l1_len, kHeaderSize, header[0], read_u32(header, 1));

  // 重写：layer0 原样，layer1 改为单段原样块（len == 633，CRC 重算）。
  std::vector<uint8_t> out;
  out.insert(out.end(), in.begin(), in.begin() + l1_off);
  append_u32(out, bwgame::data_loading::crc32_t()(header.data(), header.size()));
  append_u32(out, 1);
  append_u32(out, static_cast<uint32_t>(kHeaderSize));
  out.insert(out.end(), header.begin(), header.end());

  // layer1 之后的数据原样保留。
  const size_t tail_off = l1_data + l1_len;
  std::fprintf(stderr,
    "tail: %zu bytes copied verbatim (their framing is not rewritten yet)\n",
    in.size() - tail_off);
  out.insert(out.end(), in.begin() + tail_off, in.end());

  FILE* o = std::fopen(argv[2], "wb");
  if (!o) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  std::fwrite(out.data(), 1, out.size(), o);
  std::fclose(o);
  std::fprintf(stderr, "wrote %zu bytes to %s\n", out.size(), argv[2]);
  return 0;
}
