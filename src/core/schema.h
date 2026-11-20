#pragma once

#include <sqlite3.h>

namespace skim {
// The station database schema this build reads and writes. Add a migration
// step in schema.cpp and increase this number to change the schema.
constexpr int kStationSchemaVersion = 3;

// Brings an open station database up to kStationSchemaVersion. Each process
// may call it on open; the steps run once inside an immediate transaction.
// Throws if the database was written by a newer schema.
void MigrateStationSchema(sqlite3* db);

// Opens the database at path, creating it if needed, and migrates it.
void InitStationSchema(const char* path);

// Throws unless every table and unique index of the current schema exists in
// db with the same columns. Use it on databases from outside the station,
// such as restored snapshots, after migrating them.
void VerifyStationSchema(sqlite3* db);

// Returns the schema version recorded in an open database.
int StationSchemaVersion(sqlite3* db);
}  // namespace skim
