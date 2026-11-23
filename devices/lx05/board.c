#include "board.h"
const struct board board = {
    .model = "Xiaomi XiaoAI Speaker Play LX05",
    .project = "hassmic.lx05",
    .product = "Xiaomi XiaoAI Speaker Play LX05",
    .default_name = "Xiaomi LX05",
    .codename = "lx05",
    .keypad = "name:lx05-keys-unconfigured",
    .volume_steps = 24,
    .wake_id = "none",
    .volume_curve = board_vol_dot2,
};
