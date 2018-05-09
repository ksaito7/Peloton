//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// tile_group.cpp
//
// Identification: src/storage/tile_group.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/tile_group.h"

#include <numeric>

#include "catalog/manager.h"
#include "common/container_tuple.h"
#include "common/logger.h"
#include "common/platform.h"
#include "common/internal_types.h"
#include "storage/abstract_table.h"
#include "storage/tile.h"
#include "storage/tile_group_factory.h"
#include "storage/tile_group_header.h"
#include "storage/tuple.h"
#include "util/stringbox_util.h"

namespace peloton {
namespace storage {

TileGroup::TileGroup(BackendType backend_type,
                     TileGroupHeader *tile_group_header, AbstractTable *table,
                     const std::vector<catalog::Schema> &schemas,
                     const column_map_type &column_map, int tuple_count)
    : database_id(INVALID_OID),
      table_id(INVALID_OID),
      tile_group_id(INVALID_OID),
      backend_type(backend_type),
      tile_schemas(schemas),
      tile_group_header(tile_group_header),
      table(table),
      num_tuple_slots(tuple_count),
      column_map(column_map) {
  tile_count = tile_schemas.size();

  for (oid_t tile_itr = 0; tile_itr < tile_count; tile_itr++) {
    auto &manager = catalog::Manager::GetInstance();
    oid_t tile_id = manager.GetNextTileId();

    std::shared_ptr<Tile> tile(storage::TileFactory::GetTile(
        backend_type, database_id, table_id, tile_group_id, tile_id,
        tile_group_header, tile_schemas[tile_itr], this, tuple_count));

    // Add a reference to the tile in the tile group
    tiles.push_back(tile);
  }
}

TileGroup::~TileGroup() {
  // Drop references on all tiles

  // clean up tile group header
  delete tile_group_header;
}

oid_t TileGroup::GetTileId(const oid_t tile_id) const {
  PELOTON_ASSERT(tiles[tile_id]);
  return tiles[tile_id]->GetTileId();
}

type::AbstractPool *TileGroup::GetTilePool(const oid_t tile_id) const {
  Tile *tile = GetTile(tile_id);

  if (tile != nullptr) {
    return tile->GetPool();
  }

  return nullptr;
}

oid_t TileGroup::GetTileGroupId() const { return tile_group_id; }

// TODO: check when this function is called. --Yingjun
oid_t TileGroup::GetNextTupleSlot() const {
  return tile_group_header->GetCurrentNextTupleSlot();
}

// this function is called only when building tile groups for aggregation
// operations.
oid_t TileGroup::GetActiveTupleCount() const {
  return tile_group_header->GetActiveTupleCount();
}

//===--------------------------------------------------------------------===//
// Operations
//===--------------------------------------------------------------------===//

/**
 * Copy from tuple.
 */
void TileGroup::CopyTuple(const Tuple *tuple, const oid_t &tuple_slot_id) {
  LOG_TRACE("Tile Group Id :: %u status :: %u out of %u slots ", tile_group_id,
            tuple_slot_id, num_tuple_slots);

  oid_t tile_column_count;
  oid_t column_itr = 0;

  for (oid_t tile_itr = 0; tile_itr < tile_count; tile_itr++) {
    const catalog::Schema &schema = tile_schemas[tile_itr];
    tile_column_count = schema.GetColumnCount();

    storage::Tile *tile = GetTile(tile_itr);
    PELOTON_ASSERT(tile);
    char *tile_tuple_location = tile->GetTupleLocation(tuple_slot_id);
    PELOTON_ASSERT(tile_tuple_location);

    // NOTE:: Only a tuple wrapper
    storage::Tuple tile_tuple(&schema, tile_tuple_location);

    for (oid_t tile_column_itr = 0; tile_column_itr < tile_column_count;
         tile_column_itr++) {
      type::Value val = (tuple->GetValue(column_itr));
      tile_tuple.SetValue(tile_column_itr, val, tile->GetPool());
      column_itr++;
    }
  }
}

/**
 * Grab next slot (thread-safe) and fill in the tuple if tuple != nullptr
 *
 * Returns slot where inserted (INVALID_ID if not inserted)
 */
oid_t TileGroup::InsertTuple(const Tuple *tuple) {
  oid_t tuple_slot_id = tile_group_header->GetNextEmptyTupleSlot();

  LOG_TRACE("Tile Group Id :: %u status :: %u out of %u slots ", tile_group_id,
            tuple_slot_id, num_tuple_slots);

  // No more slots
  if (tuple_slot_id == INVALID_OID) {
    LOG_TRACE("Failed to get next empty tuple slot within tile group.");
    return INVALID_OID;
  }

  // if the input tuple is nullptr, then it means that the tuple with be filled
  // in
  // outside the function. directly return the empty slot.
  if (tuple == nullptr) {
    return tuple_slot_id;
  }

  // copy tuple.
  CopyTuple(tuple, tuple_slot_id);

  // Set MVCC info
  PELOTON_ASSERT(tile_group_header->GetTransactionId(tuple_slot_id) ==
                 INVALID_TXN_ID);
  PELOTON_ASSERT(tile_group_header->GetBeginCommitId(tuple_slot_id) == MAX_CID);
  PELOTON_ASSERT(tile_group_header->GetEndCommitId(tuple_slot_id) == MAX_CID);
  return tuple_slot_id;
}

/**
 * Grab specific slot and fill in the tuple
 * Used by recovery
 * Returns slot where inserted (INVALID_ID if not inserted)
 */
oid_t TileGroup::InsertTupleFromRecovery(cid_t commit_id, oid_t tuple_slot_id,
                                         const Tuple *tuple) {
  auto status = tile_group_header->GetEmptyTupleSlot(tuple_slot_id);

  // No more slots
  if (status == false) return INVALID_OID;

  tile_group_header->GetHeaderLock().Lock();

  cid_t current_begin_cid = tile_group_header->GetBeginCommitId(tuple_slot_id);
  if (current_begin_cid != MAX_CID && current_begin_cid > commit_id) {
    tile_group_header->GetHeaderLock().Unlock();
    return tuple_slot_id;
  }

  LOG_TRACE("Tile Group Id :: %u status :: %u out of %u slots ", tile_group_id,
            tuple_slot_id, num_tuple_slots);

  oid_t tile_column_count;
  oid_t column_itr = 0;

  for (oid_t tile_itr = 0; tile_itr < tile_count; tile_itr++) {
    const catalog::Schema &schema = tile_schemas[tile_itr];
    tile_column_count = schema.GetColumnCount();

    storage::Tile *tile = GetTile(tile_itr);
    PELOTON_ASSERT(tile);
    char *tile_tuple_location = tile->GetTupleLocation(tuple_slot_id);
    PELOTON_ASSERT(tile_tuple_location);

    // NOTE:: Only a tuple wrapper
    storage::Tuple tile_tuple(&schema, tile_tuple_location);

    for (oid_t tile_column_itr = 0; tile_column_itr < tile_column_count;
         tile_column_itr++) {
      type::Value val = (tuple->GetValue(column_itr));
      tile_tuple.SetValue(tile_column_itr, val, tile->GetPool());
      column_itr++;
    }
  }

  // Set MVCC info
  tile_group_header->SetTransactionId(tuple_slot_id, INITIAL_TXN_ID);
  tile_group_header->SetBeginCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetEndCommitId(tuple_slot_id, MAX_CID);
  tile_group_header->SetNextItemPointer(tuple_slot_id, INVALID_ITEMPOINTER);

  tile_group_header->GetHeaderLock().Unlock();

  return tuple_slot_id;
}

oid_t TileGroup::DeleteTupleFromRecovery(cid_t commit_id, oid_t tuple_slot_id) {
  auto status = tile_group_header->GetEmptyTupleSlot(tuple_slot_id);

  tile_group_header->GetHeaderLock().Lock();

  cid_t current_begin_cid = tile_group_header->GetBeginCommitId(tuple_slot_id);
  if (current_begin_cid != MAX_CID && current_begin_cid > commit_id) {
    tile_group_header->GetHeaderLock().Unlock();
    return tuple_slot_id;
  }
  // No more slots
  if (status == false) {
    tile_group_header->GetHeaderLock().Unlock();
    return INVALID_OID;
  }
  // Set MVCC info
  tile_group_header->SetTransactionId(tuple_slot_id, INVALID_TXN_ID);
  tile_group_header->SetBeginCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetEndCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetNextItemPointer(tuple_slot_id, INVALID_ITEMPOINTER);
  tile_group_header->GetHeaderLock().Unlock();
  return tuple_slot_id;
}

oid_t TileGroup::UpdateTupleFromRecovery(cid_t commit_id, oid_t tuple_slot_id,
                                         ItemPointer new_location) {
  auto status = tile_group_header->GetEmptyTupleSlot(tuple_slot_id);

  tile_group_header->GetHeaderLock().Lock();

  cid_t current_begin_cid = tile_group_header->GetBeginCommitId(tuple_slot_id);
  if (current_begin_cid != MAX_CID && current_begin_cid > commit_id) {
    tile_group_header->GetHeaderLock().Unlock();
    return tuple_slot_id;
  }

  // No more slots
  if (status == false) {
    tile_group_header->GetHeaderLock().Unlock();
    return INVALID_OID;
  }
  // Set MVCC info
  tile_group_header->SetTransactionId(tuple_slot_id, INVALID_TXN_ID);
  tile_group_header->SetBeginCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetEndCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetNextItemPointer(tuple_slot_id, new_location);
  tile_group_header->GetHeaderLock().Unlock();
  return tuple_slot_id;
}

/**
 * Grab specific slot and fill in the tuple
 * Used by checkpoint recovery
 * Returns slot where inserted (INVALID_ID if not inserted)
 */
oid_t TileGroup::InsertTupleFromCheckpoint(oid_t tuple_slot_id,
                                           const Tuple *tuple,
                                           cid_t commit_id) {
  auto status = tile_group_header->GetEmptyTupleSlot(tuple_slot_id);

  // No more slots
  if (status == false) return INVALID_OID;

  LOG_TRACE("Tile Group Id :: %u status :: %u out of %u slots ", tile_group_id,
            tuple_slot_id, num_tuple_slots);

  oid_t tile_column_count;
  oid_t column_itr = 0;

  for (oid_t tile_itr = 0; tile_itr < tile_count; tile_itr++) {
    const catalog::Schema &schema = tile_schemas[tile_itr];
    tile_column_count = schema.GetColumnCount();

    storage::Tile *tile = GetTile(tile_itr);
    PELOTON_ASSERT(tile);
    char *tile_tuple_location = tile->GetTupleLocation(tuple_slot_id);
    PELOTON_ASSERT(tile_tuple_location);

    // NOTE:: Only a tuple wrapper
    storage::Tuple tile_tuple(&schema, tile_tuple_location);

    for (oid_t tile_column_itr = 0; tile_column_itr < tile_column_count;
         tile_column_itr++) {
      type::Value val = (tuple->GetValue(column_itr));
      tile_tuple.SetValue(tile_column_itr, val, tile->GetPool());
      column_itr++;
    }
  }

  // Set MVCC info
  tile_group_header->SetTransactionId(tuple_slot_id, INITIAL_TXN_ID);
  tile_group_header->SetBeginCommitId(tuple_slot_id, commit_id);
  tile_group_header->SetEndCommitId(tuple_slot_id, MAX_CID);
  tile_group_header->SetNextItemPointer(tuple_slot_id, INVALID_ITEMPOINTER);

  return tuple_slot_id;
}

oid_t TileGroup::GetTileIdFromColumnId(oid_t column_id) {
  oid_t tile_column_id, tile_offset;
  LocateTileAndColumn(column_id, tile_offset, tile_column_id);
  return tile_offset;
}

oid_t TileGroup::GetTileColumnId(oid_t column_id) {
  oid_t tile_column_id, tile_offset;
  LocateTileAndColumn(column_id, tile_offset, tile_column_id);
  return tile_column_id;
}

type::Value TileGroup::GetValue(oid_t tuple_id, oid_t column_id) {
  PELOTON_ASSERT(tuple_id < GetNextTupleSlot());
  oid_t tile_column_id, tile_offset;
  LocateTileAndColumn(column_id, tile_offset, tile_column_id);
  return GetTile(tile_offset)->GetValue(tuple_id, tile_column_id);
}

void TileGroup::SetValue(type::Value &value, oid_t tuple_id, oid_t column_id) {
  PELOTON_ASSERT(tuple_id < GetNextTupleSlot());
  oid_t tile_column_id, tile_offset;
  LocateTileAndColumn(column_id, tile_offset, tile_column_id);
  GetTile(tile_offset)->SetValue(value, tuple_id, tile_column_id);
}

std::shared_ptr<Tile> TileGroup::GetTileReference(
    const oid_t tile_offset) const {
  PELOTON_ASSERT(tile_offset < tile_count);
  return tiles[tile_offset];
}

double TileGroup::GetSchemaDifference(
    const storage::column_map_type &new_column_map) {
  double theta = 0;
  size_t capacity = column_map.size();
  double diff = 0;

  for (oid_t col_itr = 0; col_itr < capacity; col_itr++) {
    auto &old_col = column_map.at(col_itr);
    auto &new_col = new_column_map.at(col_itr);

    // The tile don't match
    if (old_col.first != new_col.first) diff++;
  }

  // compute diff
  theta = diff / capacity;

  return theta;
}

void TileGroup::Sync() {
  // Sync the tile group data by syncing all the underlying tiles
  for (auto tile : tiles) {
    tile->Sync();
  }
}

// Serialize this tile group
void TileGroup::SerializeTo(SerializeOutput &out) {
  out.WriteInt(num_tuple_slots);
  out.WriteLong(tile_schemas.size());
  for (auto tile_schema : tile_schemas) {
    tile_schema.SerializeTo(out);
  }

  out.WriteLong(column_map.size());
  for (auto column_info : column_map) {
    oid_t column_offset = column_info.first;
    oid_t tile_offset = column_info.second.first;
    oid_t tile_column_offset = column_info.second.second;
    out.WriteInt(column_offset);
    out.WriteInt(tile_offset);
    out.WriteInt(tile_column_offset);
  }
}

// Deserialize this tile group
std::shared_ptr<TileGroup> TileGroup::DeserializeFrom(SerializeInput &in,
                                                      const oid_t database_oid,
                                                      AbstractTable *table) {
  oid_t tile_group_id = catalog::Manager::GetInstance().GetNextTileGroupId();
  oid_t allocated_tuple_count = in.ReadInt();

  size_t tile_schema_count = in.ReadLong();
  std::vector<catalog::Schema> schemas;
  for (oid_t schema_idx = 0; schema_idx < tile_schema_count; schema_idx++) {
    auto tile_schema = catalog::Schema::DeserializeFrom(in);
    schemas.push_back(*tile_schema);
  }

  column_map_type column_map;
  size_t column_map_count = in.ReadLong();
  for (oid_t column_idx = 0; column_idx < column_map_count; column_idx++) {
    oid_t column_offset = in.ReadInt();
    oid_t tile_offset = in.ReadInt();
    oid_t tile_column_offset = in.ReadInt();
    column_map[column_offset] = std::make_pair(tile_offset, tile_column_offset);
  }

  std::shared_ptr<TileGroup> tile_group(TileGroupFactory::GetTileGroup(
      database_oid, table->GetOid(), tile_group_id, table, schemas, column_map,
      allocated_tuple_count));

  return tile_group;
}

//===--------------------------------------------------------------------===//
// Utilities
//===--------------------------------------------------------------------===//

const std::string TileGroup::GetInfo() const {
  std::ostringstream os;

  os << peloton::GETINFO_DOUBLE_STAR << " TILE GROUP[#" << tile_group_id << "] "
     << peloton::GETINFO_DOUBLE_STAR << std::endl;
  os << "Database[" << database_id << "] // ";
  os << "Table[" << table_id << "] " << std::endl;
  os << (*tile_group_header) << std::endl;

  for (oid_t tile_itr = 0; tile_itr < tile_count; tile_itr++) {
    Tile *tile = GetTile(tile_itr);
    if (tile != nullptr) {
      os << std::endl
         << (*tile);
    }
  }

  // auto header = GetHeader();
  // if (header != nullptr) os << (*header);
  return peloton::StringUtil::Prefix(peloton::StringBoxUtil::Box(os.str()),
                                     GETINFO_SPACER);
}

}  // namespace storage
}  // namespace peloton
