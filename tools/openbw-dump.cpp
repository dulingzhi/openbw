// openbw-dump: 用 OpenBW 离线模拟一个 Brood War 录像，把逐帧状态导成 JSONL。
//
// 为什么用 OpenBW：录像只存命令序列、不存状态（锁步设计的必然结果），要拿到每帧
// 的单位与资源就必须跑一遍模拟。OpenBW 是开源重实现，自带 MPQ 读取与录像内嵌地图
// 支持，不需要安装游戏。
//
// 与 bwsim 的差别：bwsim 只导出对局分析用的统计量；这里导出 Pluto 训练需要的完整
// 单位状态（order、目标、冷却、血量与护盾）。字段全部取自已验证的定义，行尾标注
// 来源文件与行号。
//
// 用法：
//   openbw-dump <data-path> <replay.rep> <out.jsonl> [--max-frames N] [--visible-only]
//
// data-path 指向 StarCraft 安装目录（含 StarDat.mpq / BrooDat.mpq）。
#include "bwgame.h"
#include "replay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bwgame {

struct dump_options {
  int max_frames = -1;
  bool include_hidden = true;
};

// replay_functions 需要一个外部持有的 replay_state（replay.h:92-94）。这里只提供
// headless 所需的最小外壳，不引入 ui.h。
struct headless_replay : replay_functions {
  replay_state replay_st;
  explicit headless_replay(state& st, action_state& action_st)
    : replay_functions(st, action_st, replay_st) {}
};

// unit_type_t flags 见 data_types.h:79-95。
const char* unit_category(const unit_t* u) {
  const auto flags = u->unit_type->flags;
  if (flags & unit_type_t::flag_resource) return "resource";
  if (flags & unit_type_t::flag_building) return "building";
  if (flags & unit_type_t::flag_worker) return "worker";
  return "unit";
}

void write_unit(FILE* out, const unit_t* u) {
  // hp:             thingy_t::hp          (game_types.h:457)  fp8 -> /256
  // position:       thingy_t::position    (game_types.h:473)  像素 * 256
  // shield_points:  unit_t                (game_types.h:599)  fp8
  // order_type / order_state / order_target / cooldowns: unit_t (game_types.h:590-597)
  // status_flags:   unit_t                (game_types.h:726)
  const int target_unit = u->order_target.unit ? u->order_target.unit->index : -1;
  std::fprintf(out,
    "\"i\":%d,\"o\":%d,\"t\":%d,\"c\":\"%s\","
    "\"x\":%d,\"y\":%d,\"hp\":%d,\"sh\":%d,"
    "\"ord\":%d,\"ost\":%d,\"tgt\":%d,"
    "\"cd\":[%d,%d,%d],\"done\":%s,\"gath\":%s",
    u->index, u->owner, static_cast<int>(u->unit_type->id), unit_category(u),
    u->position.x / 256, u->position.y / 256,
    u->hp.raw_value / 256, u->shield_points.raw_value / 256,
    static_cast<int>(u->order_type->id), u->order_state, target_unit,
    u->ground_weapon_cooldown, u->air_weapon_cooldown, u->spell_cooldown,
    (u->status_flags & unit_t::status_flag_completed) ? "true" : "false",
    (u->status_flags & unit_t::status_flag_gathering) ? "true" : "false");
}

void dump_replay(const a_string& data_path, const a_string& replay_path,
  const a_string& out_path, const dump_options& opts) {
  game_player player;
  player.init(data_path);

  auto& st = player.st();
  // replay_functions 自己持有 action_state（replay.h:242），从 game_player 上取不到。
  static action_state action_st;
  headless_replay replay(st, action_st);
  std::vector<uint8_t> map_data;

  // load_replay_file 交内嵌地图给 load_map_data（replay.h:206）——这是 bwsim
  // 崩溃的那一步，OpenBW 自己处理地图块。
  replay.load_replay_file(replay_path, true, &map_data);

  FILE* out = std::fopen(out_path.c_str(), "wb");
  if (!out) {
    std::fprintf(stderr, "openbw-dump: cannot open %s\n", out_path.c_str());
    return;
  }
  std::fprintf(out,
    "{\"schema\":\"openbw-dump-v1\",\"map\":\"%s\",\"frames\":%d,\"game_type\":%d}\n",
    replay.replay_st.map_name.data(), replay.replay_st.end_frame,
    replay.replay_st.game_type);

  const int limit = opts.max_frames > 0 ? opts.max_frames : replay.replay_st.end_frame;
  int emitted = 0;
  while (static_cast<int>(st.current_frame) < limit) {
    replay.next_frame();
    ++emitted;

    // 资源在 state 上而不是 player_t 上：bwgame.h:256-257 的 current_minerals/
    // current_gas，bwgame.h:229 的 supply_used，bwgame.h:175 的 unit_type_allowed
    // 用来判断哪个槽位真正参加了游戏。
    std::fprintf(out, "{\"f\":%d,\"pl\":{", static_cast<int>(st.current_frame));
    bool first_player = true;
    for (int i = 0; i != 12; ++i) {
      const bool active = st.players[i].controller != player_t::controller_inactive;
      if (!active) continue;
      std::fprintf(out, "%s\"%d\":{\"m\":%d,\"g\":%d,\"su\":%d}",
        first_player ? "" : ",", i, st.current_minerals[i], st.current_gas[i],
        st.supply_used[i][0].raw_value / 256);
      first_player = false;
    }
    std::fprintf(out, "},\"u\":[");

    bool first_unit = true;
    const auto emit = [&](unit_t* u) {
      if (u->unit_type == nullptr) return;
      if (!first_unit) std::fputc(',', out);
      std::fputc('{', out);
      write_unit(out, u);
      std::fputc('}', out);
      first_unit = false;
    };
    for (unit_t* u : ptr(st.visible_units)) emit(u);
    if (opts.include_hidden)
      for (unit_t* u : ptr(st.hidden_units)) emit(u);

    std::fprintf(out, "]}\n");
    if (emitted % 1000 == 0) {
      std::fprintf(stderr, "openbw-dump: frame %d/%d\n", emitted, limit);
      std::fflush(out);
    }
  }
  std::fclose(out);
  std::fprintf(stderr, "openbw-dump: emitted %d frames to %s\n", emitted, out_path.c_str());
}

}  // namespace bwgame

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
      "usage: openbw-dump <data-path> <replay.rep> <out.jsonl> [--max-frames N] [--visible-only]\n");
    return 2;
  }
  bwgame::dump_options opts;
  for (int i = 4; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--max-frames") && i + 1 < argc)
      opts.max_frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--visible-only"))
      opts.include_hidden = false;
  }
  bwgame::dump_replay(argv[1], argv[2], argv[3], opts);
  return 0;
}
