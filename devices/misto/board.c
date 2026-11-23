#include "board.h"
#include <stddef.h>
const struct board board = {
    .model = "Echo 1 (EchoRoot experimental)",
    .project = "hassmic.echo-1",
    .product = "Echo 1 (hassmic)",
    .default_name = "Echo 1",
    .codename = "misto",
    .keypad = "/dev/input/event0",
    .volume_curve = board_vol_dot2,
};
