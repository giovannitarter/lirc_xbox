#include <linux/module.h>
#include <media/rc-map.h>
#include "xbox_remote_keymap.h"


static struct rc_map_table xbox[] = {
	
    { 0xa9, KEY_LEFT },
    { 0xa6, KEY_UP },
    { 0xa8, KEY_RIGHT },
    { 0xa7, KEY_DOWN },
    { 0x0b, KEY_ENTER },
    { 0xce, KEY_1 },
    { 0xcd, KEY_2 },
    { 0xcc, KEY_3 },
    { 0xcb, KEY_4 },
    { 0xca, KEY_5 },
    { 0xc9, KEY_6 },
    { 0xc8, KEY_7 },
    { 0xc7, KEY_8 },
    { 0xc6, KEY_9 },
    { 0xcf, KEY_0 },
    { 0xf7, KEY_MENU },
    { 0xd5, KEY_HOME },
    { 0xe2, KEY_REWIND },
    { 0xe3, KEY_FASTFORWARD },
    { 0xea, KEY_PLAY },
    { 0xe6, KEY_PAUSE },
    { 0xe0, KEY_STOP },
    { 0xdd, KEY_PREVIOUSSONG },
    { 0xdf, KEY_NEXTSONG },
    { 0xe5, KEY_TITLE },
    { 0xc3, KEY_INFO },
    { 0xd8, KEY_BACK }

};

static struct rc_map_list xbox_map = {
	.map = {
		.scan     = xbox,
		.size     = ARRAY_SIZE(xbox),
		.rc_proto = RC_PROTO_OTHER,
		.name     = RC_MAP_XBOX,
	}
};

static int __init init_rc_map_xbox(void)
{
	return rc_map_register(&xbox_map);
}

static void __exit exit_rc_map_xbox(void)
{
	rc_map_unregister(&xbox_map);
}

module_init(init_rc_map_xbox)
module_exit(exit_rc_map_xbox)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Giovanni Tarter <giovanni.tarter@gmail.com>");
