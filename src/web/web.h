#pragma once
namespace skim {
void Serve(const char* config);
void Serve(const char* config, const char* address, int port,
           const char* database, const char* gallery);
}  // namespace skim
