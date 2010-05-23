/*
   OpenChange Storage Abstraction Layer library
   MAPIStore FSOCPF backend

   OpenChange Project

   Copyright (C) Julien Kerihuel 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef	__MAPISTORE_FSOCPF_H
#define	__MAPISTORE_FSOCPF_H

#include <mapiproxy/libmapistore/mapistore.h>
#include <mapiproxy/libmapistore/mapistore_errors.h>
#include <dlinklist.h>
#include <dirent.h>

struct folder_list {
	uint64_t			fid;
	DIR				*dir;
};

struct folder_list_context {
	struct folder_list		*el;
	struct folder_list_context	*next;
	struct folder_list_context	*prev;
};

struct fsocpf_context {
	void				*private_data;
	char				*uri;
	struct folder_list_context	*folders;
	DIR				*dir;
};

__BEGIN_DECLS

int	mapistore_init_backend(void);

__END_DECLS

#endif /* ! __MAPISTORE_FSOCPF_H */
