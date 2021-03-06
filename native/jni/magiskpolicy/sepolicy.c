#include <dirent.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <cil/cil.h>
#include <sepol/debug.h>
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/expand.h>
#include <sepol/policydb/link.h>
#include <sepol/policydb/services.h>
#include <sepol/policydb/avrule_block.h>
#include <sepol/policydb/conditional.h>
#include <sepol/policydb/constraint.h>

#include "utils.h"
#include "magiskpolicy.h"
#include "sepolicy.h"
#include "logging.h"

policydb_t *policydb = NULL;
extern int policydb_index_decls(sepol_handle_t * handle, policydb_t * p);

static void *cmalloc(size_t s) {
	void *t = calloc(s, 1);
	if (t == NULL) {
		LOGE("Out of memory\n");
		exit(1);
	}
	return t;
}

static int get_attr(const char *type, int value) {
	type_datum_t *attr = hashtab_search(policydb->p_types.table, type);
	if (!attr)
		return 1;

	if (attr->flavor != TYPE_ATTRIB)
		return 1;

	return !! ebitmap_get_bit(&policydb->attr_type_map[attr->s.value-1], value-1);
}

static int get_attr_id(const char *type) {
	type_datum_t *attr = hashtab_search(policydb->p_types.table, type);
	if (!attr)
		return 1;

	if (attr->flavor != TYPE_ATTRIB)
		return 1;

	return attr->s.value;
}

static int set_attr(const char *type, int value) {
	type_datum_t *attr = hashtab_search(policydb->p_types.table, type);
	if (!attr)
		return 1;

	if (attr->flavor != TYPE_ATTRIB)
		return 1;

	if(ebitmap_set_bit(&policydb->type_attr_map[value-1], attr->s.value-1, 1))
		return 1;
	if(ebitmap_set_bit(&policydb->attr_type_map[attr->s.value-1], value-1, 1))
		return 1;

	return 0;
}

static int __add_rule(int s, int t, int c, int p, int effect, int not) {
	avtab_key_t key;
	avtab_datum_t *av;
	int new_rule = 0;

	key.source_type = s;
	key.target_type = t;
	key.target_class = c;
	key.specified = effect;

	av = avtab_search(&policydb->te_avtab, &key);
	if (av == NULL) {
		av = cmalloc(sizeof(*av));
		new_rule = 1;
	}

	if(not) {
		if (p < 0)
			av->data = 0U;
		else
			av->data &= ~(1U << (p - 1));
	} else {
		if (p < 0)
			av->data = ~0U;
		else
			av->data |= 1U << (p - 1);
	}

	if (new_rule) {
		if (avtab_insert(&policydb->te_avtab, &key, av)) {
			LOGW("Error inserting into avtab\n");
			return 1;
		}
		free(av);
	}

	return 0;
}

static int add_rule_auto(type_datum_t *src, type_datum_t *tgt, class_datum_t *cls, perm_datum_t *perm, int effect, int not) {
	hashtab_ptr_t cur;
	int ret = 0;

	if (src == NULL) {
		hashtab_for_each(policydb->p_types.table, &cur) {
			src = cur->datum;
			ret |= add_rule_auto(src, tgt, cls, perm, effect, not);
		}
	} else if (tgt == NULL) {
		hashtab_for_each(policydb->p_types.table, &cur) {
			tgt = cur->datum;
			ret |= add_rule_auto(src, tgt, cls, perm, effect, not);
		}
	} else if (cls == NULL) {
		hashtab_for_each(policydb->p_classes.table, &cur) {
			cls = cur->datum;
			ret |= __add_rule(src->s.value, tgt->s.value, cls->s.value, -1, effect, not);
		}
	} else {
		return __add_rule(src->s.value, tgt->s.value, cls->s.value, perm ? perm->s.value : -1, effect, not);
	}
	return ret;
}

#define ioctl_driver(x) (x>>8 & 0xFF)
#define ioctl_func(x) (x & 0xFF)

static int __add_xperm_rule(int s, int t, int c, uint16_t low, uint16_t high, int effect, int not) {
	avtab_key_t key;
	avtab_datum_t *av;
	int new_rule = 0;

	key.source_type = s;
	key.target_type = t;
	key.target_class = c;
	key.specified = effect;

	av = avtab_search(&policydb->te_avtab, &key);
	if (av == NULL) {
		av = cmalloc(sizeof(*av));
		av->xperms = cmalloc(sizeof(avtab_extended_perms_t));
		new_rule = 1;
		if (ioctl_driver(low) != ioctl_driver(high)) {
			av->xperms->specified = AVTAB_XPERMS_IOCTLDRIVER;
			av->xperms->driver = 0;
		} else {
			av->xperms->specified = AVTAB_XPERMS_IOCTLFUNCTION;
			av->xperms->driver = ioctl_driver(low);
		}
	}

	if (av->xperms->specified == AVTAB_XPERMS_IOCTLDRIVER) {
		for (unsigned i = ioctl_driver(low); i <= ioctl_driver(high); ++i) {
			if (not)
				xperm_clear(i, av->xperms->perms);
			else
				xperm_set(i, av->xperms->perms);
		}
	} else {
		for (unsigned i = ioctl_func(low); i <= ioctl_func(high); ++i) {
			if (not)
				xperm_clear(i, av->xperms->perms);
			else
				xperm_set(i, av->xperms->perms);
		}
	}

	if (new_rule) {
		if (avtab_insert(&policydb->te_avtab, &key, av)) {
			LOGW("Error inserting into avtab\n");
			return 1;
		}
		free(av);
	}

	return 0;
}

static int add_xperm_rule_auto(type_datum_t *src, type_datum_t *tgt, class_datum_t *cls,
			uint16_t low, uint16_t high, int effect, int not) {
	hashtab_ptr_t cur;
	int ret = 0;

	if (src == NULL) {
		hashtab_for_each(policydb->p_types.table, &cur) {
			src = cur->datum;
			ret |= add_xperm_rule_auto(src, tgt, cls, low, high, effect, not);
		}
	} else if (tgt == NULL) {
		hashtab_for_each(policydb->p_types.table, &cur) {
			tgt = cur->datum;
			ret |= add_xperm_rule_auto(src, tgt, cls, low, high, effect, not);
		}
	} else if (cls == NULL) {
		hashtab_for_each(policydb->p_classes.table, &cur) {
			cls = cur->datum;
			ret |= __add_xperm_rule(src->s.value, tgt->s.value, cls->s.value, low, high, effect, not);
		}
	} else {
		return __add_xperm_rule(src->s.value, tgt->s.value, cls->s.value, low, high, effect, not);
	}
	return ret;
}

int load_policydb(const char *filename) {
	struct policy_file pf;
	void *map;
	size_t size;
	int ret;

	if (policydb)
		destroy_policydb();

	policydb = cmalloc(sizeof(*policydb));

	mmap_ro(filename, &map, &size);

	policy_file_init(&pf);
	pf.type = PF_USE_MEMORY;
	pf.data = map;
	pf.len = size;
	if (policydb_init(policydb)) {
		LOGE("policydb_init: Out of memory!\n");
		return 1;
	}
	ret = policydb_read(policydb, &pf, 0);
	if (ret) {
		LOGE("error(s) encountered while parsing configuration\n");
		return 1;
	}

	munmap(map, size);

	return 0;
}

int compile_split_cil() {
	DIR *dir;
	struct dirent *entry;
	char path[128];

	struct cil_db *db = NULL;
	sepol_policydb_t *pdb = NULL;
	void *addr;
	size_t size;

	cil_db_init(&db);
	cil_set_mls(db, 1);
	cil_set_multiple_decls(db, 1);
	cil_set_disable_neverallow(db, 1);
	cil_set_target_platform(db, SEPOL_TARGET_SELINUX);
	cil_set_policy_version(db, POLICYDB_VERSION_XPERMS_IOCTL);
	cil_set_attrs_expand_generated(db, 0);

	// plat
	mmap_ro(SPLIT_PLAT_CIL, &addr, &size);
	if (cil_add_file(db, SPLIT_PLAT_CIL, addr, size))
		return 1;
	LOGD("cil_add[%s]\n", SPLIT_PLAT_CIL);
	munmap(addr, size);

	// mapping
	char plat[10];
	int fd = open(SPLIT_NONPLAT_VER, O_RDONLY | O_CLOEXEC);
	plat[read(fd, plat, sizeof(plat)) - 1] = '\0';
	sprintf(path, SPLIT_PLAT_MAPPING, plat);
	mmap_ro(path, &addr, &size);
	if (cil_add_file(db, path, addr, size))
		return 1;
	LOGD("cil_add[%s]\n", path);
	munmap(addr, size);
	close(fd);

	// nonplat
	dir = opendir(NONPLAT_POLICY_DIR);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strend(entry->d_name, ".cil") == 0) {
			sprintf(path, NONPLAT_POLICY_DIR "%s", entry->d_name);
			mmap_ro(path, &addr, &size);
			if (cil_add_file(db, path, addr, size))
				return 1;
			LOGD("cil_add[%s]\n", path);
			munmap(addr, size);
		}
	}
	closedir(dir);

	if (cil_compile(db))
		return 1;
	if (cil_build_policydb(db, &pdb))
		return 1;

	cil_db_destroy(&db);
	policydb = &pdb->p;
	return 0;
}

int dump_policydb(const char *filename) {
	int fd, ret;
	void *data = NULL;
	size_t len;
	policydb_to_image(NULL, policydb, &data, &len);
	if (data == NULL) {
		LOGE("Fail to dump policy image!");
		return 1;
	}

	fd = creat(filename, 0644);
	if (fd < 0) {
		LOGE("Can't open '%s':  %s\n", filename, strerror(errno));
		return 1;
	}
	ret = xwrite(fd, data, len);
	close(fd);
	if (ret < 0)
		return 1;
	return 0;
}

void destroy_policydb() {
	policydb_destroy(policydb);
	free(policydb);
	policydb = NULL;
}

int create_domain(const char *d) {
	symtab_datum_t *src = hashtab_search(policydb->p_types.table, d);
	if(src) {
		LOGW("Domain %s already exists\n", d);
		return 0;
	}

	type_datum_t *typedatum = (type_datum_t *) malloc(sizeof(type_datum_t));
	type_datum_init(typedatum);
	typedatum->primary = 1;
	typedatum->flavor = TYPE_TYPE;

	uint32_t value = 0;
	symtab_insert(policydb, SYM_TYPES, strdup(d), typedatum, SCOPE_DECL, 1, &value);
	typedatum->s.value = value;

	if (ebitmap_set_bit(&policydb->global->branch_list->declared.scope[SYM_TYPES], value - 1, 1)) {
		return 1;
	}

	policydb->type_attr_map = realloc(policydb->type_attr_map, sizeof(ebitmap_t) * policydb->p_types.nprim);
	policydb->attr_type_map = realloc(policydb->attr_type_map, sizeof(ebitmap_t) * policydb->p_types.nprim);
	ebitmap_init(&policydb->type_attr_map[value-1]);
	ebitmap_init(&policydb->attr_type_map[value-1]);
	ebitmap_set_bit(&policydb->type_attr_map[value-1], value-1, 1);

	src = hashtab_search(policydb->p_types.table, d);
	if(!src)
		return 1;

	if(policydb_index_decls(NULL, policydb))
		return 1;

	if(policydb_index_classes(policydb))
		return 1;

	if(policydb_index_others(NULL, policydb, 0))
		return 1;

	//Add the domain to all roles
	for(unsigned i=0; i<policydb->p_roles.nprim; ++i) {
		//Not sure all those three calls are needed
		ebitmap_set_bit(&policydb->role_val_to_struct[i]->types.negset, value-1, 0);
		ebitmap_set_bit(&policydb->role_val_to_struct[i]->types.types, value-1, 1);
		type_set_expand(&policydb->role_val_to_struct[i]->types, &policydb->role_val_to_struct[i]->cache, policydb, 0);
	}

	return set_attr("domain", value);
}

int set_domain_state(const char *s, int state) {
	type_datum_t *type;
	hashtab_ptr_t cur;
	if (s == NULL) {
		hashtab_for_each(policydb->p_types.table, &cur) {
			type = cur->datum;
			if (ebitmap_set_bit(&policydb->permissive_map, type->s.value, state)) {
				LOGW("Could not set bit in permissive map\n");
				return 1;
			}
		}
	} else {
		type = hashtab_search(policydb->p_types.table, s);
		if (type == NULL) {
			LOGW("type %s does not exist\n", s);
			return 1;
		}
		if (ebitmap_set_bit(&policydb->permissive_map, type->s.value, state)) {
			LOGW("Could not set bit in permissive map\n");
			return 1;
		}
	}

	return 0;
}

int add_transition(const char *s, const char *t, const char *c, const char *d) {
	type_datum_t *src, *tgt, *def;
	class_datum_t *cls;

	avtab_key_t key;
	avtab_datum_t *av;
	int new_rule = 0;

	src = hashtab_search(policydb->p_types.table, s);
	if (src == NULL) {
		LOGW("source type %s does not exist\n", s);
		return 1;
	}
	tgt = hashtab_search(policydb->p_types.table, t);
	if (tgt == NULL) {
		LOGW("target type %s does not exist\n", t);
		return 1;
	}
	cls = hashtab_search(policydb->p_classes.table, c);
	if (cls == NULL) {
		LOGW("class %s does not exist\n", c);
		return 1;
	}
	def = hashtab_search(policydb->p_types.table, d);
	if (def == NULL) {
		LOGW("default type %s does not exist\n", d);
		return 1;
	}

	key.source_type = src->s.value;
	key.target_type = tgt->s.value;
	key.target_class = cls->s.value;
	key.specified = AVTAB_TRANSITION;
	av = avtab_search(&policydb->te_avtab, &key);
	if (av == NULL) {
		av = cmalloc(sizeof(*av));
		new_rule = 1;
	}

	av->data = def->s.value;

	if (new_rule) {
		if (avtab_insert(&policydb->te_avtab, &key, av)) {
			LOGW("Error inserting into avtab\n");
			return 1;
		}
		free(av);
	}
	return 0;
}

int add_file_transition(const char *s, const char *t, const char *c, const char *d,
						const char *filename) {
	type_datum_t *src, *tgt, *def;
	class_datum_t *cls;

	src = hashtab_search(policydb->p_types.table, s);
	if (src == NULL) {
		LOGW("source type %s does not exist\n", s);
		return 1;
	}
	tgt = hashtab_search(policydb->p_types.table, t);
	if (tgt == NULL) {
		LOGW("target type %s does not exist\n", t);
		return 1;
	}
	cls = hashtab_search(policydb->p_classes.table, c);
	if (cls == NULL) {
		LOGW("class %s does not exist\n", c);
		return 1;
	}
	def = hashtab_search(policydb->p_types.table, d);
	if (def == NULL) {
		LOGW("default type %s does not exist\n", d);
		return 1;
	}

	filename_trans_t trans_key;
	trans_key.stype = src->s.value;
	trans_key.ttype = tgt->s.value;
	trans_key.tclass = cls->s.value;
	trans_key.name = (char *) filename;

	filename_trans_datum_t *trans_datum;
	trans_datum = hashtab_search(policydb->p_types.table, (hashtab_key_t) &trans_key);

	if (trans_datum == NULL) {
		trans_datum = cmalloc(sizeof(*trans_datum));
		hashtab_insert(policydb->filename_trans, (hashtab_key_t) &trans_key, trans_datum);
	}

	// Overwrite existing
	trans_datum->otype = def->s.value;
	return 0;
}

int add_typeattribute(const char *domainS, const char *attr) {
	type_datum_t *domain;

	domain = hashtab_search(policydb->p_types.table, domainS);
	if (domain == NULL) {
		LOGW("source type %s does not exist\n", domainS);
		return 1;
	}

	set_attr(attr, domain->s.value);

	int typeId = get_attr_id(attr);
	//Now let's update all constraints!
	//(kernel doesn't support (yet?) type_names rules)
	for(int i=0; i<policydb->p_classes.nprim; ++i) {
		class_datum_t *cl = policydb->class_val_to_struct[i];
		for(constraint_node_t *n = cl->constraints; n ; n=n->next) {
			for(constraint_expr_t *e = n->expr; e; e=e->next) {
				if(e->expr_type == CEXPR_NAMES) {
					if(ebitmap_get_bit(&e->type_names->types, typeId-1)) {
						ebitmap_set_bit(&e->names, domain->s.value-1, 1);
					}
				}
			}
		}
	}
	return 0;
}

int add_rule(const char *s, const char *t, const char *c, const char *p, int effect, int n) {
	type_datum_t *src = NULL, *tgt = NULL;
	class_datum_t *cls = NULL;
	perm_datum_t *perm = NULL;

	if (s) {
		src = hashtab_search(policydb->p_types.table, s);
		if (src == NULL) {
			LOGW("source type %s does not exist\n", s);
			return 1;
		}
	}

	if (t) {
		tgt = hashtab_search(policydb->p_types.table, t);
		if (tgt == NULL) {
			LOGW("target type %s does not exist\n", t);
			return 1;
		}
	}

	if (c) {
		cls = hashtab_search(policydb->p_classes.table, c);
		if (cls == NULL) {
			LOGW("class %s does not exist\n", c);
			return 1;
		}
	}

	if (p) {
		if (c == NULL) {
			LOGW("No class is specified, cannot add perm [%s] \n", p);
			return 1;
		}

		perm = hashtab_search(cls->permissions.table, p);
		if (perm == NULL && cls->comdatum != NULL) {
			perm = hashtab_search(cls->comdatum->permissions.table, p);
		}
		if (perm == NULL) {
			LOGW("perm %s does not exist in class %s\n", p, c);
			return 1;
		}
	}
	return add_rule_auto(src, tgt, cls, perm, effect, n);
}

int add_xperm_rule(const char *s, const char *t, const char *c, const char *range, int effect,
				   int n) {
	type_datum_t *src = NULL, *tgt = NULL;
	class_datum_t *cls = NULL;

	if (s) {
		src = hashtab_search(policydb->p_types.table, s);
		if (src == NULL) {
			LOGW("source type %s does not exist\n", s);
			return 1;
		}
	}

	if (t) {
		tgt = hashtab_search(policydb->p_types.table, t);
		if (tgt == NULL) {
			LOGW("target type %s does not exist\n", t);
			return 1;
		}
	}

	if (c) {
		cls = hashtab_search(policydb->p_classes.table, c);
		if (cls == NULL) {
			LOGW("class %s does not exist\n", c);
			return 1;
		}
	}

	uint16_t low, high;

	if (range) {
		if (strchr(range, '-')){
			sscanf(range, "%hx-%hx", &low, &high);
		} else {
			sscanf(range, "%hx", &low);
			high = low;
		}
	} else {
		low = 0;
		high = 0xFFFF;
	}

	return add_xperm_rule_auto(src, tgt, cls, low, high, effect, n);
}
