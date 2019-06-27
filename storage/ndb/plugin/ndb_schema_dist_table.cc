/*
   Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Implements the interface defined in
#include "storage/ndb/plugin/ndb_schema_dist_table.h"

#include <sstream>

#include "storage/ndb/plugin/ndb_thd_ndb.h"

const std::string Ndb_schema_dist_table::DB_NAME = "mysql";
const std::string Ndb_schema_dist_table::TABLE_NAME = "ndb_schema";

const char *Ndb_schema_dist_table::COL_DB = "db";
const char *Ndb_schema_dist_table::COL_NAME = "name";
const char *Ndb_schema_dist_table::COL_QUERY = "query";
const char *Ndb_schema_dist_table::COL_ID = "id";
const char *Ndb_schema_dist_table::COL_VERSION = "version";
static const char *COL_SLOCK = "slock";
static const char *COL_NODEID = "node_id";
static const char *COL_EPOCH = "epoch";
static const char *COL_TYPE = "type";
static const char *COL_SCHEMA_OP_ID = "schema_op_id";

Ndb_schema_dist_table::Ndb_schema_dist_table(Thd_ndb *thd_ndb)
    : Ndb_util_table(thd_ndb, DB_NAME, TABLE_NAME, true) {}

Ndb_schema_dist_table::~Ndb_schema_dist_table() {}

bool Ndb_schema_dist_table::check_schema() const {
  // db
  // varbinary, at least 63 bytes long
  // NOTE! The 63 bytes length for the db and name column is a legacy bug
  // which doesn't have enough room for MySQL's max identitfier size. For
  // backwards compatiblity reasons it's allowed to use such a schema
  // distribution table but not all identifiers will be possible to distribute.
  if (!(check_column_exist(COL_DB) && check_column_varbinary(COL_DB) &&
        check_column_minlength(COL_DB, 63))) {
    return false;
  }

  // name
  // varbinary, at least 63 bytes long
  if (!(check_column_exist(COL_NAME) && check_column_varbinary(COL_NAME) &&
        check_column_minlength(COL_NAME, 63))) {
    return false;
  }
  // Check that db + name is the primary key, otherwise pk operations
  // using that key won't work
  if (!check_primary_key({COL_DB, COL_NAME})) {
    return false;
  }

  // slock
  // binary, need room for at least 32 bytes(i.e 32*8 bits for 256 nodes)
  if (!(check_column_exist(COL_SLOCK) && check_column_binary(COL_SLOCK) &&
        check_column_minlength(COL_SLOCK, 32))) {
    return false;
  }

  // query
  // blob
  if (!(check_column_exist(COL_QUERY) && check_column_blob(COL_QUERY))) {
    return false;
  }

  // nodeid
  // unsigned int
  if (!(check_column_exist(COL_NODEID) && check_column_unsigned(COL_NODEID))) {
    return false;
  }

  // epoch
  // unsigned bigint
  if (!(check_column_exist(COL_EPOCH) && check_column_bigunsigned(COL_EPOCH))) {
    return false;
  }

  // id
  // unsigned int
  if (!(check_column_exist(COL_ID) && check_column_unsigned(COL_ID))) {
    return false;
  }

  // version
  // unsigned int
  if (!(check_column_exist(COL_VERSION) &&
        check_column_unsigned(COL_VERSION))) {
    return false;
  }

  // type
  // unsigned int
  if (!(check_column_exist(COL_TYPE) && check_column_unsigned(COL_TYPE))) {
    return false;
  }

  // schema_op_id
  // unsigned int
  if (!check_column_exist(COL_SCHEMA_OP_ID)) {
    // This is an optional column added in 8.0.17. Functionality depending on
    // this column will be conditional but warnings are pushed to alert the user
    // that upgrade is eventually necessary
    ;
  } else {
    // Column exists, check proper type
    if (!(check_column_unsigned(COL_SCHEMA_OP_ID) &&
          check_column_nullable(COL_SCHEMA_OP_ID, true))) {
      return false;
    }
  }

  return true;
}

bool Ndb_schema_dist_table::check_column_identifier_limit(
    const char *column_name, const std::string &identifier) const {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("column_name: '%s', identifier: '%s'", column_name,
                       identifier.c_str()));

  if (!check_column_exist(column_name)) {
    return false;
  }

  const int max_length = DBUG_EVALUATE_IF("ndb_schema_dist_63byte_limit", 63,
                                          get_column_max_length(column_name));

  if (identifier.length() > static_cast<size_t>(max_length)) {
    push_warning("Identifier length exceeds the %d byte limit", max_length);
    return false;
  }
  return true;
}

bool Ndb_schema_dist_table::define_table_ndb(NdbDictionary::Table &new_table,
                                             unsigned mysql_version) const {
  // Set metadata for backwards compatibility support, earlier versions
  // will see what they expect and can connect to NDB properly. The physical
  // table in NDB may be extended to support new functionality but should still
  // be possible to use.
  constexpr uint8 legacy_metadata[418] = {
      0x01, 0x00, 0x00, 0x00, 0x6a, 0x22, 0x00, 0x00, 0x96, 0x01, 0x00, 0x00,
      0x78, 0x9c, 0xed, 0xd8, 0x3d, 0x4b, 0xc3, 0x50, 0x14, 0x06, 0xe0, 0x37,
      0x89, 0x89, 0x37, 0xb1, 0xd4, 0x0f, 0x82, 0x83, 0xd3, 0x75, 0x10, 0xb4,
      0x83, 0x6d, 0x45, 0xdd, 0xa4, 0xa6, 0x28, 0x5a, 0xfc, 0x2a, 0xa5, 0x83,
      0x9d, 0xc4, 0x36, 0x01, 0xeb, 0x47, 0xab, 0xad, 0x0a, 0x0e, 0x4a, 0xfd,
      0x29, 0xce, 0x0e, 0x8e, 0x0e, 0x0e, 0x42, 0x07, 0x7f, 0x88, 0xbf, 0x43,
      0x7a, 0x3d, 0x89, 0x55, 0x3a, 0xba, 0x45, 0xf0, 0x3c, 0x4b, 0xce, 0x79,
      0x39, 0xe1, 0xde, 0x33, 0x26, 0x3d, 0xcd, 0x49, 0x1a, 0xc0, 0x98, 0x06,
      0x64, 0x80, 0xba, 0xd6, 0xc5, 0x0f, 0x3d, 0x05, 0x1b, 0x30, 0xc3, 0x52,
      0x7c, 0x67, 0x75, 0x9a, 0x9b, 0x79, 0x03, 0xf6, 0xa3, 0x2e, 0x09, 0xa4,
      0xd3, 0x80, 0x04, 0x63, 0x8c, 0x31, 0xc6, 0x18, 0x63, 0x8c, 0x31, 0xc6,
      0xfe, 0x32, 0x4d, 0x07, 0x1c, 0x7a, 0xde, 0x41, 0x37, 0xa8, 0xeb, 0xd0,
      0xf7, 0xbd, 0xb6, 0x9a, 0x83, 0xde, 0xf1, 0xbe, 0x0a, 0x55, 0x2c, 0x15,
      0x76, 0xbc, 0x52, 0x45, 0xc5, 0x7d, 0x51, 0x16, 0x3f, 0x07, 0x0d, 0xbf,
      0x5a, 0x3b, 0xbd, 0x6a, 0x5f, 0x06, 0xad, 0x79, 0xea, 0x65, 0xd1, 0x2b,
      0x95, 0x0b, 0xe5, 0xc2, 0xde, 0xae, 0xcc, 0x57, 0xe4, 0xd6, 0x7a, 0x45,
      0xa6, 0x53, 0xd3, 0x4b, 0x99, 0xe5, 0x6c, 0x56, 0x7a, 0xdb, 0x1b, 0x7b,
      0xa5, 0x42, 0x79, 0x73, 0x47, 0xae, 0xc8, 0x05, 0x99, 0x4a, 0xcb, 0xd9,
      0x39, 0x68, 0x13, 0x71, 0x2f, 0xc0, 0x18, 0x63, 0x8c, 0x31, 0xc6, 0x18,
      0x63, 0xff, 0xd7, 0xb1, 0x8e, 0xb1, 0xb8, 0xef, 0x10, 0x27, 0x0d, 0x36,
      0x6e, 0xf1, 0x4e, 0x55, 0x17, 0x8b, 0x3f, 0x69, 0x11, 0x93, 0xfd, 0xea,
      0x16, 0x8e, 0xad, 0xbb, 0x73, 0xf2, 0x97, 0x30, 0x04, 0xc3, 0xaf, 0xc2,
      0x84, 0xd9, 0x38, 0x3c, 0x0b, 0x60, 0xc1, 0x6a, 0x9f, 0x36, 0x6b, 0x27,
      0x18, 0x86, 0x75, 0x71, 0x15, 0xb4, 0x6e, 0x20, 0x20, 0x1a, 0x4d, 0x3f,
      0x38, 0xa8, 0xfb, 0x74, 0xb0, 0x15, 0x9c, 0x37, 0x6b, 0x47, 0xf4, 0x59,
      0x6d, 0x50, 0x3b, 0x02, 0x71, 0x1d, 0xb4, 0xda, 0xf5, 0x66, 0x03, 0x09,
      0x98, 0x97, 0x37, 0xe7, 0x01, 0x86, 0x8c, 0x5c, 0x0e, 0xd1, 0x1f, 0x19,
      0xba, 0xc9, 0x68, 0x0e, 0x30, 0x4d, 0x0a, 0xbc, 0x81, 0xc0, 0xb2, 0xe8,
      0xcc, 0xfb, 0x7e, 0xd0, 0xa3, 0x60, 0xd8, 0x12, 0x02, 0x0f, 0xc0, 0xf8,
      0x9a, 0x0b, 0x7c, 0x50, 0x20, 0x84, 0xe3, 0xe0, 0x11, 0x98, 0x0a, 0x27,
      0x0c, 0x01, 0xd8, 0x96, 0xeb, 0xe2, 0x09, 0xc8, 0x87, 0x01, 0x0d, 0xc3,
      0x31, 0x68, 0xe2, 0x79, 0x60, 0x62, 0x24, 0x7c, 0xe5, 0x65, 0x20, 0x48,
      0x98, 0x14, 0xbc, 0x0e, 0x04, 0xca, 0xaf, 0xaa, 0x70, 0x41, 0x15, 0x6d,
      0xa7, 0xa2, 0xd5, 0x54, 0x7f, 0x2f, 0x15, 0x2d, 0xa5, 0xa8, 0xe8, 0xaf,
      0xa3, 0xc2, 0x5d, 0x14, 0x3e, 0x01, 0x4d, 0x53, 0x5e, 0x81};
  if (new_table.setFrm(legacy_metadata, sizeof(legacy_metadata)) != 0) {
    push_warning("Failed to set legacy metadata");
    return false;
  }

  new_table.setForceVarPart(true);

  // Allow table to be read+write also in single user mode
  new_table.setSingleUserMode(NdbDictionary::Table::SingleUserModeReadWrite);

  {
    // db VARBINARY(63) NOT NULL
    NdbDictionary::Column col_db(COL_DB);
    col_db.setType(NdbDictionary::Column::Varbinary);
    col_db.setLength(63);
    col_db.setNullable(false);
    col_db.setPrimaryKey(true);
    if (!define_table_add_column(new_table, col_db)) return false;
  }

  {
    // name VARBINARY(63) NOT NULL
    NdbDictionary::Column col_name(COL_NAME);
    col_name.setType(NdbDictionary::Column::Varbinary);
    col_name.setLength(63);
    col_name.setNullable(false);
    col_name.setPrimaryKey(true);
    if (!define_table_add_column(new_table, col_name)) return false;
  }

  {
    // slock BINARY(32) NOT NULL
    NdbDictionary::Column col_slock(COL_SLOCK);
    col_slock.setType(NdbDictionary::Column::Binary);
    col_slock.setLength(32);
    col_slock.setNullable(false);
    if (!define_table_add_column(new_table, col_slock)) return false;
  }

  {
    // query BLOB NOT NULL
    NdbDictionary::Column col_query(COL_QUERY);
    col_query.setType(NdbDictionary::Column::Blob);
    col_query.setInlineSize(256);
    col_query.setPartSize(2000);
    col_query.setStripeSize(0);
    col_query.setNullable(false);
    if (!define_table_add_column(new_table, col_query)) return false;
  }

  {
    // node_id INT UNSIGNED NOT NULL
    NdbDictionary::Column col_nodeid(COL_NODEID);
    col_nodeid.setType(NdbDictionary::Column::Unsigned);
    col_nodeid.setNullable(false);
    if (!define_table_add_column(new_table, col_nodeid)) return false;
  }

  {
    // epoch BIGINT UNSIGNED NOT NULL
    NdbDictionary::Column col_epoch(COL_EPOCH);
    col_epoch.setType(NdbDictionary::Column::Bigunsigned);
    col_epoch.setNullable(false);
    if (!define_table_add_column(new_table, col_epoch)) return false;
  }

  {
    // id INT UNSIGNED NOT NULL
    NdbDictionary::Column col_id(COL_ID);
    col_id.setType(NdbDictionary::Column::Unsigned);
    col_id.setNullable(false);
    if (!define_table_add_column(new_table, col_id)) return false;
  }

  {
    // version INT UNSIGNED NOT NULL
    NdbDictionary::Column col_version(COL_VERSION);
    col_version.setType(NdbDictionary::Column::Unsigned);
    col_version.setNullable(false);
    if (!define_table_add_column(new_table, col_version)) return false;
  }

  {
    // type INT UNSIGNED NOT NULL
    NdbDictionary::Column col_type(COL_TYPE);
    col_type.setType(NdbDictionary::Column::Unsigned);
    col_type.setNullable(false);
    if (!define_table_add_column(new_table, col_type)) return false;
  }

  if (mysql_version >= 80017) {
    // schema_op_id INT UNSIGNED NULL
    NdbDictionary::Column col_schema_op_id(COL_SCHEMA_OP_ID);
    col_schema_op_id.setType(NdbDictionary::Column::Unsigned);
    col_schema_op_id.setNullable(true);  // NULL!
    if (!define_table_add_column(new_table, col_schema_op_id)) return false;
  }

  return true;
}

bool Ndb_schema_dist_table::need_upgrade() const {
  // Check that 'schema_op_id' column exists. If exists, it's used for sending
  // the schema_op_id from client to participants who can then use it when
  // replying using ndb_schema_result (if they support that table)
  if (!have_schema_op_id_column()) {
    return true;
  }
  return false;
}

bool Ndb_schema_dist_table::drop_events_in_NDB() const {
  // Drop the default event on ndb_schema table
  if (!drop_event_in_NDB("REPL$mysql/ndb_schema")) return false;

  // Legacy event on ndb_schema table, drop since it might
  // have been created(although ages ago)
  if (!drop_event_in_NDB("REPLF$mysql/ndb_schema")) return false;

  return true;
}

std::string Ndb_schema_dist_table::define_table_dd() const {
  std::stringstream ss;
  ss << "CREATE TABLE " << db_name() << "." << table_name() << "(\n";
  ss << "db VARBINARY(63) NOT NULL,"
        "name VARBINARY(63) NOT NULL,"
        "slock BINARY(32) NOT NULL,"
        "query BLOB NOT NULL,"
        "node_id INT UNSIGNED NOT NULL,"
        "epoch BIGINT UNSIGNED NOT NULL,"
        "id INT UNSIGNED NOT NULL,"
        "version INT UNSIGNED NOT NULL,"
        "type INT UNSIGNED NOT NULL,";
  if (have_schema_op_id_column()) {
    ss << "schema_op_id INT UNSIGNED NULL,";
  }
  ss << "PRIMARY KEY USING HASH (db,name)"
     << ") ENGINE=ndbcluster CHARACTER SET latin1";
  return ss.str();
}

int Ndb_schema_dist_table::get_slock_bytes() const {
  return get_column_max_length(COL_SLOCK);
}

bool Ndb_schema_dist_table::have_schema_op_id_column() const {
  const NdbDictionary::Table *ndb_tab = get_table();
  return ndb_tab->getColumn(COL_SCHEMA_OP_ID);
}
