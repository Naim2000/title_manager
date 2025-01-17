void print_this_dumb_header(void);
void print_this_dumb_line(void);

typedef const struct menu_item_list {
	void         (*print_header)(const void*, int cursor, int count);
	const void*   header_ptr;
	const void*   items;
	size_t        item_size;
	unsigned int  num_items;
    unsigned int  max_items;
	const char*  (*get_name)(const void *, char buffer[256]);
	void         (*select)(const void *);
} menu_item_list_t;


void ItemMenu(menu_item_list_t* list);
