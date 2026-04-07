#include "common/rag/rag_index_service.h"

#include "common/rag/ollama_engine_client.h"
#include "common/rag/ollama_engine_service.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

#include "common/rag/rag_index_service_common.inl"
#include "common/rag/rag_index_service_snippet.inl"
#include "common/rag/rag_index_service_lexical.inl"
#include "common/rag/rag_index_service_scan.inl"
#include "common/rag/rag_index_service_retrieval.inl"
