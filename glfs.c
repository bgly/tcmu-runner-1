/*
 * Copyright 2015, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
*/

#define _GNU_SOURCE

#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <scsi/scsi.h>
#include <glusterfs/api/glfs.h>

#include "tcmu-runner.h"

#define ALLOWED_BSOFLAGS (O_SYNC | O_DIRECT | O_RDWR | O_LARGEFILE)

#define GLUSTER_PORT 24007

struct glfs_state {
	char *name;
	glfs_t *fs;
	glfs_fd_t *gfd;
	char *servername;
	char *volname;
	char *pathname;

	unsigned int block_size;

	/*
	 * Current tcmu helper API reports WCE=1, but doesn't
	 * implement inquiry VPD 0xb2, so clients will not know UNMAP
	 * or WRITE_SAME are supported. TODO: fix this
	 */
};

/*
 * Break image string into server, volume, and path components.
 * Returns -1 on failure.
 */
static int parse_imagepath(
	char *cfgstring,
	char **servername,
	char **volname,
	char **pathname)
{
	char *origp = strdup(cfgstring);
	char *t_servername = NULL;
	char *t_volname = NULL;
	char *t_pathname = NULL;
	char *p, *sep;

	if (!origp)
		goto fail;

	/* part before '@' is the volume name */
	p = origp;
	sep = strchr(p, '@');
	if (!sep)
		goto fail;

	*sep = '\0';
	t_volname = strdup(p);
	if (!t_volname)
		goto fail;

	/* part between '@' and 1st '/' is the server name */
	p = sep + 1;
	sep = strchr(p, '/');
	if (!sep)
		goto fail;

	*sep = '\0';
	t_servername = strdup(p);
	if (!t_servername)
		goto fail;

	/* The rest is the path name */
	p = sep + 1;
	t_pathname = strdup(p);
	if (!t_pathname)
		goto fail;

	if (!strlen(t_servername) || !strlen(t_volname) || !strlen(t_pathname))
		goto fail;

	free(origp);
	*servername = t_servername;
	*volname = t_volname;
	*pathname = t_pathname;

	return 0;

fail:
	free(t_volname);
	free(t_servername);
	free(t_pathname);
	free(origp);

	return -1;
}

static bool glfs_check_config(const char *cfgstring, char **reason)
{
	char *path;
	char *servername = NULL;
	char *volname = NULL;
	char *pathname = NULL;
	glfs_t *fs = NULL;
	glfs_fd_t *gfd = NULL;
	struct stat st;
	int ret;
	bool result = true;

	path = strchr(cfgstring, '/');
	if (!path) {
		if (asprintf(reason, "No path found") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}
	path += 1; /* get past '/' */

	if (parse_imagepath(path, &servername, &volname, &pathname) == -1) {
		if (asprintf(reason, "Invalid imagepath") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	/* Actually attempt to open the volume to verify things are working */
	/* TODO: consolidate this with v. similar tcmu_glfs_open code? */
	fs = glfs_new(volname);
	if (!fs) {
		if (asprintf(reason, "glfs_new failed") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	ret = glfs_set_volfile_server(fs, "tcp", servername,
				      GLUSTER_PORT);
	if (ret) {
		if (asprintf(reason, "glfs_set_volfile_server failed: %m") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	ret = glfs_init(fs);
	if (ret) {
		if (asprintf(reason, "glfs_init failed: %m") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	gfd = glfs_open(fs, pathname, ALLOWED_BSOFLAGS);
	if (!gfd) {
		if (asprintf(reason, "glfs_open failed: %m") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

	ret = glfs_lstat(fs, pathname, &st);
	if (ret) {
		if (asprintf(reason, "glfs_lstat failed: %m") == -1)
			*reason = NULL;
		result = false;
		goto done;
	}

done:
	if (gfd)
		glfs_close(gfd);
	if (fs)
		glfs_fini(fs);
	free(servername);
	free(volname);
	free(pathname);

	return result;
}

static int tcmu_glfs_open(struct tcmu_device *dev)
{
	struct glfs_state *gfsp;
	int ret = 0;
	char *config;
	struct stat st;
	int attribute;

	gfsp = calloc(1, sizeof(*gfsp));
	if (!gfsp)
		return -ENOMEM;

	tcmu_set_dev_private(dev, gfsp);

	attribute = tcmu_get_attribute(dev, "hw_block_size");
	if (attribute == -1) {
		errp("Could not get hw_block_size setting\n");
		goto fail;
	}
	gfsp->block_size = attribute;

	config = strchr(tcmu_get_dev_cfgstring(dev), '/');
	if (!config) {
		errp("no configuration found in cfgstring\n");
		goto fail;
	}
	config += 1; /* get past '/' */

	if (parse_imagepath(config, &gfsp->servername, &gfsp->volname, &gfsp->pathname) == -1) {
		errp("servername, volname, or pathname not set\n");
		goto fail;
	}

	gfsp->fs = glfs_new(gfsp->volname);
	if (!gfsp->fs) {
		errp("glfs_new failed\n");
		goto fail;
	}

	ret = glfs_set_volfile_server(gfsp->fs, "tcp", gfsp->servername,
				      GLUSTER_PORT);
	if (ret) {
		errp("glfs_set_volfile_server failed: %m\n");
		goto fail;
	}


	ret = glfs_init(gfsp->fs);
	if (ret) {
		errp("glfs_init failed: %m\n");
		goto fail;
	}

	gfsp->gfd = glfs_open(gfsp->fs, gfsp->pathname, ALLOWED_BSOFLAGS);
	if (!gfsp->gfd) {
		errp("glfs_open failed: %m\n");
		goto fail;
	}

	ret = glfs_lstat(gfsp->fs, gfsp->pathname, &st);
	if (ret) {
		errp("glfs_lstat failed: %m\n");
		goto fail;
	}

	if (st.st_size != tcmu_get_device_size(dev)) {
		errp("device size and backing size disagree: "
		       "device %lld backing %lld\n",
		       tcmu_get_device_size(dev),
		       (long long) st.st_size);
		goto fail;
	}

	return 0;

fail:
	if (gfsp->gfd)
		glfs_close(gfsp->gfd);
	if (gfsp->fs)
		glfs_fini(gfsp->fs);
	free(gfsp->volname);
	free(gfsp->pathname);
	free(gfsp->servername);
	free(gfsp);

	return -EIO;
}

static void tcmu_glfs_close(struct tcmu_device *dev)
{
	struct glfs_state *gfsp = tcmu_get_dev_private(dev);

	glfs_close(gfsp->gfd);
	glfs_fini(gfsp->fs);
	free(gfsp->volname);
	free(gfsp->pathname);
	free(gfsp->servername);
	free(gfsp);
}

static int set_medium_error(uint8_t *sense)
{
	return tcmu_set_sense_data(sense, MEDIUM_ERROR, ASC_READ_ERROR, NULL);
}

/*
 * Return scsi status or TCMU_NOT_HANDLED
 */
int tcmu_glfs_handle_cmd(
	struct tcmu_device *dev,
	struct tcmulib_cmd *tcmulib_cmd)
{
	uint8_t *cdb = tcmulib_cmd->cdb;
	struct iovec *iovec = tcmulib_cmd->iovec;
	size_t iov_cnt = tcmulib_cmd->iov_cnt;
	uint8_t *sense = tcmulib_cmd->sense_buf;
	struct glfs_state *state = tcmu_get_dev_private(dev);
	uint8_t cmd;

	glfs_fd_t *gfd = state->gfd;
	int ret;
	uint32_t length;
	int result = SAM_STAT_GOOD;
	char *tmpbuf;
	uint64_t offset = state->block_size * tcmu_get_lba(cdb);
	uint32_t tl     = state->block_size * tcmu_get_xfer_length(cdb);
	int do_verify = 0;
	uint32_t cmp_offset;
	ret = length = 0;

	cmd = cdb[0];

	switch (cmd) {
	case INQUIRY:
		return tcmu_emulate_inquiry(dev, cdb, iovec, iov_cnt, sense);
		break;
	case TEST_UNIT_READY:
		return tcmu_emulate_test_unit_ready(cdb, iovec, iov_cnt, sense);
		break;
	case SERVICE_ACTION_IN_16:
		if (cdb[1] == READ_CAPACITY_16) {
			long long size;
			unsigned long long num_lbas;

			size = tcmu_get_device_size(dev);
			if (size == -1) {
				errp("Could not get device size\n");
				return TCMU_NOT_HANDLED;
			}

			num_lbas = size / state->block_size;

			return tcmu_emulate_read_capacity_16(num_lbas, state->block_size,
							     cdb, iovec, iov_cnt, sense);
		} else {
			return TCMU_NOT_HANDLED;
		}
		break;
	case READ_CAPACITY:
		if ((cdb[1] & 0x01) || (cdb[8] & 0x01)) {
			/* Reserved bits for MM logical units */
			return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
						   ASC_INVALID_FIELD_IN_CDB,
						   NULL);
		} else {
			long long size;
			unsigned long long num_lbas;

			size = tcmu_get_device_size(dev);
			if (size == -1) {
				errp("Could not get device size\n");
				return TCMU_NOT_HANDLED;
			}

			num_lbas = size / state->block_size;
			return tcmu_emulate_read_capacity_10(num_lbas,
							     state->block_size,
							     cdb, iovec,
							     iov_cnt, sense);
		}
	case MODE_SENSE:
	case MODE_SENSE_10:
		return tcmu_emulate_mode_sense(cdb, iovec, iov_cnt, sense);
		break;
	case START_STOP:
		return tcmu_emulate_start_stop(dev, cdb, sense);
		break;
	case MODE_SELECT:
	case MODE_SELECT_10:
		return tcmu_emulate_mode_select(cdb, iovec, iov_cnt, sense);
		break;
	case COMPARE_AND_WRITE:
		/* Blocks are transferred twice, first the set that
		 * we compare to the existing data, and second the set
		 * to write if the compare was successful.
		 */
		length = tl / 2;

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = tcmu_set_sense_data(sense, HARDWARE_ERROR,
						     ASC_INTERNAL_TARGET_FAILURE, NULL);
			break;
		}

		ret = glfs_pread(gfd, tmpbuf, length, offset, SEEK_SET);

		if (ret != length) {
			result = set_medium_error(sense);
			free(tmpbuf);
			break;
		}

		cmp_offset = tcmu_compare_with_iovec(tmpbuf, iovec, length);
		if (cmp_offset != -1) {
			result = tcmu_set_sense_data(sense, MISCOMPARE,
						     ASC_MISCOMPARE_DURING_VERIFY_OPERATION,
						     &cmp_offset);
			free(tmpbuf);
			break;
		}

		free(tmpbuf);

		tcmu_seek_in_iovec(iovec, length);
		goto write;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		if (cdb[1] & 0x2)
			result = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
						     ASC_INVALID_FIELD_IN_CDB, NULL);
		else
			glfs_fdatasync(gfd);
		break;
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
	case WRITE_VERIFY_16:
		do_verify = 1;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		length = tl;
write:
		ret = glfs_pwritev(gfd, iovec, iov_cnt, offset, ALLOWED_BSOFLAGS);

		if (ret == length) {
			/* Sync if FUA */
			if ((cmd != WRITE_6) && (cdb[1] & 0x8))
				glfs_fdatasync(gfd);
		} else {
			errp("Error on write %x %x\n", ret, length);
			result = set_medium_error(sense);
			break;
		}

		if (!do_verify)
			break;

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = tcmu_set_sense_data(sense, HARDWARE_ERROR,
						     ASC_INTERNAL_TARGET_FAILURE, NULL);
			break;
		}

		ret = glfs_pread(gfd, tmpbuf, length, offset, ALLOWED_BSOFLAGS);

		if (ret != length) {
			result = set_medium_error(sense);
			free(tmpbuf);
			break;
		}

		cmp_offset = tcmu_compare_with_iovec(tmpbuf, iovec, length);
		if (cmp_offset != -1)
			result = tcmu_set_sense_data(sense, MISCOMPARE,
					    ASC_MISCOMPARE_DURING_VERIFY_OPERATION,
					    &cmp_offset);
		free(tmpbuf);
		break;

	case WRITE_SAME:
	case WRITE_SAME_16:
		errp("WRITE_SAME called, but has vpd b2 been implemented?\n");
		result = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					     ASC_INVALID_FIELD_IN_CDB, NULL);
		break;

#if 0
		/* WRITE_SAME used to punch hole in file */
		if (cdb[1] & 0x08) {
			ret = glfs_discard(gfd, offset, tl);
			if (ret != 0) {
				result = tcmu_set_sense_data(sense, HARDWARE_ERROR,
						    ASC_INTERNAL_TARGET_FAILURE, NULL);
			}
			break;
		}
		while (tl > 0) {
			size_t blocksize = state->block_size;
			uint32_t val32;
			uint64_t val64;

			assert(iovec->iov_len >= 8);

			switch (cdb[1] & 0x06) {
			case 0x02: /* PBDATA==0 LBDATA==1 */
				val32 = htobe32(offset);
				memcpy(iovec->iov_base, &val32, 4);
				break;
			case 0x04: /* PBDATA==1 LBDATA==0 */
				/* physical sector format */
				/* hey this is wrong val! But how to fix? */
				val64 = htobe64(offset);
				memcpy(iovec->iov_base, &val64, 8);
				break;
			default:
				/* FIXME */
				errp("PBDATA and LBDATA set!!!\n");
			}

			ret = glfs_pwritev(gfd, iovec, blocksize,
					offset, ALLOWED_BSOFLAGS);

			if (ret != blocksize)
				result = set_medium_error(sense);

			offset += blocksize;
			tl     -= blocksize;
		}
		break;
#endif
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		length = tcmu_iovec_length(iovec, iov_cnt);
		ret = glfs_preadv(gfd, iovec, iov_cnt, offset, SEEK_SET);

		if (ret != length) {
			errp("Error on read %x %x\n", ret, length);
			result = set_medium_error(sense);
		}
		break;
	case UNMAP:
		/* TODO: implement UNMAP */
		result = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					     ASC_INVALID_FIELD_IN_CDB, NULL);
		break;
	default:
		result = TCMU_NOT_HANDLED;
		break;
	}

	dbgp("io done %p %x %d %u\n", cdb, cmd, result, length);

	if (result == TCMU_NOT_HANDLED)
		dbgp("io not handled %p %x %x %d %d %llu\n",
		     cdb, result, cmd, ret, length, (unsigned long long)offset);
	else if (result != SAM_STAT_GOOD) {
		errp("io error %p %x %x %d %d %llu\n",
		     cdb, result, cmd, ret, length, (unsigned long long)offset);
	}

	return result;
}

static const char glfs_cfg_desc[] =
	"glfs config string is of the form:\n"
	"\"volume@hostname/filename\"\n"
	"where:\n"
	"  volume:    The volume on the Gluster server\n"
	"  hostname:  The server's hostname\n"
	"  filename:  The backing file";

struct tcmur_handler glfs_handler = {
	.name = "Gluster glfs handler",
	.subtype = "glfs",
	.cfg_desc = glfs_cfg_desc,

	.check_config = glfs_check_config,

	.open = tcmu_glfs_open,
	.close = tcmu_glfs_close,
	.handle_cmd = tcmu_glfs_handle_cmd,
};

/* Entry point must be named "handler_init". */
void handler_init(void)
{
	tcmur_register_handler(&glfs_handler);
}
