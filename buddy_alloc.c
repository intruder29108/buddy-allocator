/*
 * buddy_alloc.c: Simple buddy allocator.
 *
 */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#define MAX_BUDDY_ENTRIES						(1024)
#define msg_err(msg, ...)						\
	printf("[ERR]: " msg "\n", ##__VA_ARGS__)
#define msg_info(msg, ...)						\
	printf("[INFO]: " msg "\n", ##__VA_ARGS__)

struct prog_args_t {
	bool help;
	bool is_verbose;
	int max_order;
	int page_size;
	int start_addr;
	int alloc_size;
	int alloc_loop;
	int sub_loop;
};

struct buddy_entry_t {
	unsigned int start_addr;
	int order;
	int idx;
	bool is_used;
	struct buddy_entry_t *buddy;
	struct buddy_entry_t *parent;
};

struct buddy_list_t {
	int used_count;
	int free_count;
	struct buddy_entry_t *used_entries[MAX_BUDDY_ENTRIES];
	struct buddy_entry_t *free_entries[MAX_BUDDY_ENTRIES];
};

struct buddy_allocator_t {
	int max_order;
	int page_size;
	int shift_count;
	unsigned int start_addr;
	struct buddy_list_t *buddy_list;
};

static const struct option long_options[] = {
	{"help",	0, 0, 'h'},
	{"verbose",	0, 0, 'v'},
	{"max-order",	1, 0, 'o'},
	{"page-size",	1, 0, 'p'},
	{"start-addr",	1, 0, 's'},
	{"loop",	1, 0, 'l'},
	{"sub-loop",	1, 0, 'n'},
	{"alloc-size",	1, 0, 'a'},
	{NULL,		0, 0,  0 }
};

static const char *option_str = "hvo:p:s:l:a:n:";
static struct prog_args_t prog_args;

static void buddy_add_entry(struct buddy_allocator_t *allocator,
		struct buddy_entry_t *entry)
{
	struct buddy_list_t *buddy_list = &allocator->buddy_list[entry->order];

	entry->is_used = false;
	entry->idx = buddy_list->free_count;
	buddy_list->free_entries[buddy_list->free_count++] = entry;
}

static void buddy_remove_entry(struct buddy_allocator_t *allocator,
		struct buddy_entry_t *entry)
{
	struct buddy_list_t *buddy_list = &allocator->buddy_list[entry->order];

	buddy_list->free_count--;
	entry->is_used = true;
	entry->idx = buddy_list->used_count;
	buddy_list->used_entries[buddy_list->used_count++] = entry;
}

static int buddy_allocator_init(struct buddy_allocator_t *allocator)
{
	struct buddy_entry_t *first_entry;
	allocator->buddy_list = (struct buddy_list_t *)calloc(sizeof(struct buddy_list_t),
			allocator->max_order + 1);

	allocator->shift_count = ffs(allocator->page_size) - 1;
	first_entry = (struct buddy_entry_t *)calloc(sizeof(*first_entry), 1);
	first_entry->start_addr = allocator->start_addr;
	first_entry->order = allocator->max_order;
	first_entry->buddy = NULL;
	first_entry->parent = NULL;
	for (int i = 0; i <= allocator->max_order; i++) {
		allocator->buddy_list[i].free_count = 0;
		allocator->buddy_list[i].used_count = 0;
		memset(allocator->buddy_list[i].free_entries, 0,
				sizeof(allocator->buddy_list[i].free_entries));
		memset(allocator->buddy_list[i].used_entries, 0,
				sizeof(allocator->buddy_list[i].used_entries));
	}
	buddy_add_entry(allocator, first_entry);

	return 0;
}

static struct buddy_entry_t* buddy_split_entry(struct buddy_allocator_t *allocator,
		struct buddy_entry_t *entry)
{
	struct buddy_entry_t *new_entry[2];
	int new_order = entry->order - 1;
	int buddy_size = allocator->page_size * new_order;

	for ( int i = 0; i < 2; i++ ) {
		new_entry[i] = (struct buddy_entry_t *)\
			       calloc(sizeof(*entry), 1);
		new_entry[i]->start_addr = entry->start_addr + i * buddy_size;
		new_entry[i]->order = new_order;
		new_entry[i]->parent = entry;
		buddy_add_entry(allocator, new_entry[i]);
	}
	new_entry[0]->buddy = new_entry[1];
	new_entry[1]->buddy = new_entry[0];

	return new_entry[1];
}

static struct buddy_entry_t* buddy_alloc_internal(struct buddy_allocator_t *allocator, int order)
{
	struct buddy_list_t *buddy_list = &allocator->buddy_list[order];
	struct buddy_entry_t *buddy_entry = NULL;

	if (order > allocator->max_order) {
		return NULL;
	}

	if (buddy_list->free_count > 0) {
		buddy_entry = buddy_list->free_entries[buddy_list->free_count - 1];
		buddy_remove_entry(allocator, buddy_entry);
		buddy_entry->is_used = true;
	} else {
		buddy_entry = buddy_alloc_internal(allocator, order + 1);
		if (buddy_entry != NULL) {
			buddy_entry = buddy_split_entry(allocator, buddy_entry);
			buddy_remove_entry(allocator, buddy_entry);
		}
	}

	return buddy_entry;
}

static void buddy_free_internal(struct buddy_allocator_t *allocator, struct buddy_entry_t *entry)
{
	struct buddy_entry_t *buddy = entry->buddy;
	struct buddy_list_t *buddy_list = &allocator->buddy_list[entry->order];


	for (int i = entry->idx; i < buddy_list->used_count - 1; i++) {
			buddy_list->used_entries[i] = buddy_list->used_entries[i + 1];
	}
	buddy_list->used_count--;

	if (buddy) {
		if (buddy->is_used) {
			buddy_add_entry(allocator, entry);
		} else {
			for (int i =  entry->idx; i < buddy_list->free_count - 1; i++) {
				buddy_list->free_entries[i] = buddy_list->free_entries[i + 1];
			}
			buddy_list->free_count--;
			free(buddy);
			free(entry);
			if (entry->parent != NULL) {
				buddy_free_internal(allocator, entry->parent);
			}
		}
	} else {
		buddy_add_entry(allocator, entry);
	}
}

static struct buddy_entry_t* buddy_alloc(struct buddy_allocator_t *allocator, int size)
{
	int page_order = (size >> allocator->shift_count);

	return buddy_alloc_internal(allocator, page_order);
}

static void buddy_free(struct buddy_allocator_t *allocator, struct buddy_entry_t *entry)
{
	buddy_free_internal(allocator, entry);
}

static void buddy_print_statistics(struct buddy_allocator_t *allocator)
{
	int i;
	const char *decorator = "===============================================================";
	int width = strlen(decorator);
	char *field_names[] = {"Order", "Free Entries", "Used Entries"};
	int num_fields = sizeof(field_names)/sizeof(*field_names);
	int field_width = width / num_fields;
	char *header = (char *)malloc(width + 1);
	int idx = 0;

	for (i = 0; i < num_fields; i++) {
		idx += sprintf(&header[idx], "%*s", field_width, field_names[i]);
	}


	printf("%s\n", decorator);
	printf("%s\n", header);
	printf("%s\n", decorator);
	for (i = 0; i <= allocator->max_order; i++) {
		printf("%*d%*d%*d\n", field_width, i,
				field_width, allocator->buddy_list[i].free_count,
				field_width, allocator->buddy_list[i].used_count);

	}

}
static int parse_args(int argc, char **argv)
{
	int c, option_index;

	while (1) {

		c = getopt_long(argc, argv, option_str, long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
			case 'h':
				prog_args.help = true;
				break;
			case 'v':
				prog_args.is_verbose = true;
				break;
			case 'o':
				prog_args.max_order = strtol(optarg, NULL, 10);
				if (prog_args.max_order == 0) {
					msg_err("invaliid max-order");
					return -1;
				}
				break;
			case 's':
				prog_args.start_addr = strtol(optarg, NULL, 10);
				if (prog_args.start_addr < 0) {
					msg_err("invalid start-addr");
					return -1;
				}
				break;
			case 'p':
				prog_args.page_size = strtol(optarg, NULL, 10);
				if (prog_args.page_size == 0 ||\
						(prog_args.page_size & (prog_args.page_size - 1))) {
					msg_err("invalid page-size");
					return -1;
				}
				break;
			case 'l':
				prog_args.alloc_loop = strtol(optarg, NULL, 10);
				if (prog_args.alloc_loop < 0) {
					msg_err("invalid alloc-loop");
					return -1;
				}
				break;
			case 'a':
				prog_args.alloc_size = strtol(optarg, NULL, 10);
				if (prog_args.alloc_size < 0) {
					msg_err("invalid alloc-size");
					return -1;
				}
				break;
			case 'n':
				prog_args.sub_loop = strtol(optarg, NULL, 10);
				if (prog_args.sub_loop < 0) {
					msg_err("invalid sub-loop");
					return -1;
				}
				break;

			default:
				return -1;
		}
	}

	return 0;
}

static const char* usage_string = "buddy_alloc -o max-order -s start-addr -p page-size -l alloc-loop -a alloc-size -n sub-loop";
static void print_usage()
{
	msg_info("USAGE: %s", usage_string);
}

int main(int argc, char *argv[])
{
	struct buddy_allocator_t alloc = {0};
	struct buddy_entry_t **alloc_entries;
	int count = 0;

	if (parse_args(argc, argv) != 0) {
		print_usage();
		return -1;
	}
	if (prog_args.help) {
		print_usage();
		return 0;
	}

	if (prog_args.alloc_size < prog_args.page_size) {
		msg_err("alloc_size shoud be greater than: %d bytes",
				prog_args.page_size);
		return -1;
	}
	alloc.max_order = prog_args.max_order;
	alloc.page_size = prog_args.page_size;
	alloc.start_addr = prog_args.start_addr;
	buddy_allocator_init(&alloc);

	msg_info("buddy allocator initialized");
	msg_info("max_order(%d), page_size(%d), start_addr(0x%x)",
			alloc.max_order, alloc.page_size, alloc.start_addr);
	buddy_print_statistics(&alloc);
	alloc_entries = (struct buddy_entry_t **)calloc(sizeof(*alloc_entries),
			prog_args.alloc_loop * prog_args.sub_loop);
	for (int i = 0; i < prog_args.alloc_loop; i++) {
		int size = prog_args.alloc_size << i;

		for (int j = 0; j < prog_args.sub_loop; j++) {
			alloc_entries[count] = buddy_alloc(&alloc, size);
			if(alloc_entries[count] == NULL) {
				msg_err("allocation(%d) failed", i);
			}
			count++;
		}
	}
	buddy_print_statistics(&alloc);
	msg_info("made %d allocations", count);
	for (int i = count; i >= 0; i--) {
		if (alloc_entries[i] != NULL) {
			buddy_free(&alloc, alloc_entries[i]);
		}
	}
	buddy_print_statistics(&alloc);


	return 0;
}
