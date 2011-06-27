#define _GNU_SOURCE
#include <stdio.h>
#include <confuse.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <dirent.h>

#include "genimage.h"

/*
 * TODO:
 *
 * - add documentation
 * - implement extraargs for the different image handlers (i.e. jffs2-extraargs = "", genext2fs-extraargs = "")
 * - make more configurable (path to programs and the like)
 * - implement missing image types (cpio, iso)
 * - free memory after usage
 * - make more failsafe (does flashtype exist where necessary)
 * - check for recursive image references
 * - implement command line switches (--verbose, --dry-run, --config=)
 * - implement a log(struct image *, const char *format, ...) function
 * - implement different compression types for tar (depending on file suffix or explicit switches)
 *
 */
static struct image_handler *handlers[] = {
	&jffs2_handler,
	&flash_handler,
	&tar_handler,
	&ubi_handler,
	&ubifs_handler,
	&hdimage_handler,
	&ext2_handler,
	&file_handler,
};

static int image_get_type(struct image *image, cfg_t *cfg)
{
	int num = 0, i, x;

	for (i = 0; i < ARRAY_SIZE(handlers); i++) {
		struct image_handler *handler = handlers[i];

		x = cfg_size(cfg, handler->type);
		if (x)
			image->handler = handler;
		num += x;
	}

	if (num > 1) {
		fprintf(stderr, "multiple image types given\n");
		exit (1);
	}

	if (num < 1) {
		fprintf(stderr, "no image type given\n");
		exit (1);
	}

	image->imagesec = cfg_getsec(cfg, image->handler->type);

	return 0;
}

#if 0
static void dump_image(struct image *i)
{
	printf("file: %-25s "
		"name: %-20s "
		"size: %-10lld "
		"offset: %-10lld "
		"mountpoint: %-20s "
		"partition_type: %-3d\n", i->file, i->name, i->size, i->offset, 
			i->mountpoint, i->partition_type);
	i->handler->handler(i);
}
#endif

unsigned long long cfg_getint_suffix(cfg_t *sec, const char *name)
{
	const char *str = cfg_getstr(sec, name);
	unsigned long long val = 0;

	if (str)
		val = strtoul_suffix(str, NULL, 0);

	return val;
}

static cfg_opt_t partition_opts[] = {
	CFG_STR("offset", NULL, CFGF_NONE),
	CFG_STR("size", NULL, CFGF_NONE),
	CFG_INT("partition-type", 0, CFGF_NONE),
	CFG_STR("image", NULL, CFGF_NONE),
	CFG_BOOL("autoresize", 0, CFGF_NONE),
	CFG_END()
};

static cfg_opt_t image_common_opts[] = {
	CFG_STR("name", NULL, CFGF_NONE),
	CFG_STR("size", NULL, CFGF_NONE),
	CFG_STR("mountpoint", NULL, CFGF_NONE),
	CFG_STR("offset", NULL, CFGF_NONE),
	CFG_STR("flashtype", NULL, CFGF_NONE),
	CFG_SEC("partition", partition_opts, CFGF_MULTI | CFGF_TITLE),
};

static cfg_opt_t flashchip_opts[] = {
	CFG_STR("pebsize", "", CFGF_NONE),
	CFG_STR("lebsize", "", CFGF_NONE),
	CFG_STR("numpebs", "", CFGF_NONE),
	CFG_STR("minimum-io-unit-size", "", CFGF_NONE),
	CFG_STR("vid-header-offset", "", CFGF_NONE),
	CFG_STR("sub-page-size", "", CFGF_NONE),
	CFG_END()
};

static LIST_HEAD(images);

struct image *image_get(const char *filename)
{
	struct image *image;

	list_for_each_entry(image, &images, list) {
		if (!strcmp(image->file, filename))
			return image;
	}
	return NULL;
}

static int image_generate(struct image *image)
{
	int ret;
	struct partition *part;

	if (image->done)
		return 0;

	list_for_each_entry(part, &image->partitions, list) {
		struct image *child;
		child = image_get(part->image);
		if (!child) {
			image_error(image, "could not find %s\n", part->image);
			return -EINVAL;
		}
		ret = image_generate(child);
		if (ret) {
			image_error(image, "could not generate %s\n", child->file);
			return ret;
		}
	}

	if (image->handler->generate) {
		ret = image->handler->generate(image);
	} else {
		error("no generate function for %s\n", image->file);
		return -EINVAL;
	}

	if (ret)
		return ret;

	image->done = 1;

	return 0;
}

const char *mountpath(struct image *image)
{
	return image->mp->mountpath; 
}

static LIST_HEAD(flashlist);

int parse_flashes(cfg_t *cfg)
{
	int num_flashes;
	int i;

	num_flashes = cfg_size(cfg, "flash");

	for (i = 0; i < num_flashes; i++) {
		cfg_t *flashsec = cfg_getnsec(cfg, "flash", i);
		struct flash_type *flash = xzalloc(sizeof *flash);

		flash->name = cfg_title(flashsec);
		flash->pebsize = cfg_getint_suffix(flashsec, "pebsize");
		flash->lebsize = cfg_getint_suffix(flashsec, "lebsize");
		flash->numpebs = cfg_getint_suffix(flashsec, "numpebs");
		flash->minimum_io_unit_size = cfg_getint_suffix(flashsec, "minimum-io-unit-size");
		flash->vid_header_offset = cfg_getint_suffix(flashsec, "vid-header-offset");
		flash->sub_page_size = cfg_getint_suffix(flashsec, "sub-page-size");
		list_add_tail(&flash->list, &flashlist);
	}

	return 0;
}

struct flash_type *flash_type_get(const char *name)
{
	struct flash_type *flash;

	list_for_each_entry(flash, &flashlist, list) {
		if (!strcmp(flash->name, name))
			return flash;
	}
	return NULL;
}

int image_set_flash_type(struct image *image, struct flash_type *type)
{
	if (!image->flash_type) {
		image->flash_type = type;
		return 0;
	}

	if (image->flash_type != type)
		return -EBUSY;

	return 0;
}

static int parse_partitions(struct image *image, cfg_t *imagesec)
{
	struct partition *part;
	int num_partitions;
	int i;

	num_partitions = cfg_size(imagesec, "partition");

	for (i = 0; i < num_partitions; i++) {
		cfg_t *partsec = cfg_getnsec(imagesec, "partition", i);

		part = xzalloc(sizeof *part);
		part->name = cfg_title(partsec);
		list_add_tail(&part->list, &image->partitions);
		part->size = cfg_getint_suffix(partsec, "size");
		part->offset = cfg_getint_suffix(partsec, "offset");
		part->partition_type = cfg_getint(partsec, "partition-type");
		part->image = cfg_getstr(partsec, "image");
		part->autoresize = cfg_getbool(partsec, "autoresize");
        }

	return 0;
}

static int set_flash_type(void)
{
	struct image *image;

	list_for_each_entry(image, &images, list) {
		struct partition *part;
		if (!image->flash_type)
			continue;
		list_for_each_entry(part, &image->partitions, list) {
			struct image *i;
			i = image_get(part->image);
			if (!i)
				return -EINVAL;
			if (i->flash_type) {
				if (i->flash_type != image->flash_type) {
					printf("conflicting flash types: %s has flashtype %s whereas %s has flashtype %s\n",
							i->file, i->flash_type->name, image->file, image->flash_type->name);
					return -EINVAL;
				}
			} else {
				i->flash_type = image->flash_type;
			}
		}
	}
	return 0;
}

static LIST_HEAD(mountpoints);

static struct mountpoint *add_mountpoint(const char *path)
{
	struct mountpoint *mp;

	list_for_each_entry(mp, &mountpoints, list) {
		if (!strcmp(mp->path, path))
			return mp;
	}

	mp = xzalloc(sizeof(*mp));
	mp->path = strdup(path);
	asprintf(&mp->mountpath, "%s/%s", tmppath(), mp->path);
	list_add_tail(&mp->list, &mountpoints);

	return mp;
}

static void add_root_mountpoint(void)
{
	struct mountpoint *mp;

	mp = xzalloc(sizeof(*mp));
	mp->path = strdup("");
	asprintf(&mp->mountpath, "%s/root", tmppath());
	list_add_tail(&mp->list, &mountpoints);
}

static int collect_mountpoints(void)
{
	struct image *image;
	struct mountpoint *mp;
	int ret;

	add_root_mountpoint();

	ret = systemp(NULL, "mkdir -p %s", tmppath());
	if (ret)
		return ret;

	ret = systemp(NULL, "cp -a %s %s/root", rootpath(), tmppath());
	if (ret)
		return ret;

	list_for_each_entry(image, &images, list) {
		if (image->mountpoint)
			image->mp = add_mountpoint(image->mountpoint);
	}

	list_for_each_entry(mp, &mountpoints, list) {
		if (!strlen(mp->path))
			continue;
		ret = systemp(NULL, "mv %s/root/%s %s", tmppath(), mp->path, tmppath());
		if (ret)
			return ret;
	}

	return 0;
}

static void check_tmp_path(void)
{
	const char *tmp = tmppath();
	int ret;
	DIR *dir;
	int i = 0;

	if (!tmp) {
		fprintf(stderr, "tmppath not set. aborting\n");
		exit(1);
	}

	dir = opendir(tmp);
	if (!dir) {
		ret = systemp(NULL, "mkdir -p %s", tmppath());
		if (ret)
			exit(1);
		closedir(dir);
		return;
	}

	while (1) {
		if (!readdir(dir))
			break;
		i++;
		if (i > 2) {
			fprintf(stderr, "tmppath '%s' exists and is not empty\n",
					tmp);
			exit(1);
		}
	}
}

static void cleanup(void)
{
	systemp(NULL, "rm -rf %s/*", tmppath());
}

static cfg_opt_t top_opts[] = {
	CFG_SEC("image", NULL, CFGF_MULTI | CFGF_TITLE),
	CFG_SEC("flash", flashchip_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_SEC("config", NULL, CFGF_MULTI),
	CFG_FUNC("include", &cfg_include),
	CFG_END()
};

int main(int argc, char *argv[])
{
	int i;
	int num_images;
	int ret;
	cfg_opt_t *imageopts = xzalloc((ARRAY_SIZE(image_common_opts) +
				ARRAY_SIZE(handlers) + 1) * sizeof(cfg_opt_t));;
	int start;
	struct image *image;
	char *str;
	cfg_t *cfg;
	struct partition *part;

	cfg_opt_t image_end[] = {
		CFG_END()
	};

	memcpy(imageopts, image_common_opts, sizeof(image_common_opts));

	start = ARRAY_SIZE(image_common_opts);
	for (i = 0; i < ARRAY_SIZE(handlers); i++) {
		struct image_handler *handler = handlers[i];
		cfg_opt_t image_tmp[] = {
			CFG_SEC("dummy", NULL, CFGF_MULTI),
		};

		image_tmp[0].name = handler->type;
		image_tmp[0].subopts = handler->opts;
		
		memcpy(&imageopts[start + i], image_tmp, sizeof(cfg_opt_t));
	}

	memcpy(&imageopts[start + i], &image_end[0], sizeof(cfg_opt_t));

	top_opts[0].subopts = imageopts;

	init_config();
	top_opts[2].subopts = get_config_opts();

	cfg = cfg_init(top_opts, CFGF_NONE);
	if (cfg_parse(cfg, "test.config") == CFG_PARSE_ERROR)
		goto err_out;

	set_config_opts(argc, argv, cfg);

	check_tmp_path();

	ret = systemp(NULL, "rm -rf %s/*", tmppath());
	if (ret)
		goto err_out;

	parse_flashes(cfg);

	num_images = cfg_size(cfg, "image");

	for (i = 0; i < num_images; i++) {
		cfg_t *imagesec = cfg_getnsec(cfg, "image", i);
		image = xzalloc(sizeof *image);
		INIT_LIST_HEAD(&image->partitions);
		list_add_tail(&image->list, &images);
		image->file = cfg_title(imagesec);
		image->name = cfg_getstr(imagesec, "name");
		image->size = cfg_getint_suffix(imagesec, "size");
		image->offset = cfg_getint_suffix(imagesec, "offset");
		image->mountpoint = cfg_getstr(imagesec, "mountpoint");
		if (image->mountpoint && *image->mountpoint == '/')
			image->mountpoint++;
		str = cfg_getstr(imagesec, "flashtype");
		if (str)
			image->flash_type = flash_type_get(str);
		image_get_type(image, imagesec);
		parse_partitions(image, imagesec);
        }

	/* check if each partition has a corresponding image */
	list_for_each_entry(image, &images, list) {
		list_for_each_entry(part, &image->partitions, list) {
			struct image *i = image_get(part->image);
			if (!i) {
				image_error(image, "no rule to generate %s\n", part->image);
				goto err_out;
			}
		}
	}

	/* propagate flash types to partitions */
	ret = set_flash_type();
	if (ret)
		goto err_out;

	ret = collect_mountpoints();
	if (ret)
		goto err_out;

	list_for_each_entry(image, &images, list) {
		if (image->handler->setup) {
			ret = image->handler->setup(image, image->imagesec);
			if (ret)
				goto err_out;
		}
	}

	list_for_each_entry(image, &images, list) {
		ret = image_generate(image);
		if (ret) {
			printf("failed to generate %s\n", image->file);
			break;
		}
	}

	cleanup();
	exit(0);

err_out:
	cleanup();
	exit(1);
}
