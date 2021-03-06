/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <minos/vmodule.h>
#include <minos/sched.h>
#include <minos/arch.h>
#include <minos/virt.h>

extern void virqs_init(void);
extern int static_vms_init(void);
extern int create_static_vms(void);

extern struct virt_config virt_config;
struct virt_config *mv_config = &virt_config;

void parse_memtags(void)
{
	int i;
	size_t size = mv_config->nr_memtag;
	struct memtag *memtags = mv_config->memtags;

	for (i = 0; i < size; i++)
		register_memory_region(&memtags[i]);
}

int virt_init(void)
{
	int ret;

	vmodules_init();
	virqs_init();
	parse_memtags();

	ret = create_static_vms();
	if (!ret)
		return -ENOENT;

	static_vms_init();

	return 0;
}
