# Vulkan MCP Server Search Guide

## Overview
The Vulkan MCP server provides access to 3200+ Vulkan 1.4 API entities through Claude. This guide covers effective search strategies and known quirks.

## Setup (Windows + WSL)

The server now resolves the Vulkan registry path cross‑platform and supports environment overrides.

- VULKAN_REGISTRY_XML: Absolute path to `vk.xml`.
  - Windows example: `VULKAN_REGISTRY_XML=D:\\dev\\vulkan\\xml\\vk.xml`
  - WSL example: `export VULKAN_REGISTRY_XML=/mnt/d/dev/vulkan/xml/vk.xml`
  - On WSL, Windows paths like `D:\\...` are accepted and normalized to `/mnt/d/...`.
- VULKAN_DOCS_ROOT: Folder containing one of: `xml/vk.xml`, `vulkan_docs/xml/vk.xml`, or `index/vk.xml`.
- VULKAN_MCP_DB (optional): Path for the generated SQLite database (defaults next to this script).

Repo‑relative fallback (no env vars):
- The server auto‑detects `markdown/vulkan_offline_mirror/vulkan_docs/xml/vk.xml` in this repository.

Original Windows layout (no env vars required):
- Place the server at `E:\coding_projects\ue_manual_mirror\vulkan_offline_mirror\mcp_server\vulkan_mcp_fixed_final.py`.
- Keep the registry at `E:\coding_projects\ue_manual_mirror\vulkan_offline_mirror\vulkan_docs\xml\vk.xml`.
- The server auto‑detects that sibling path. Optionally set:
  - `VULKAN_DOCS_ROOT=E:\coding_projects\ue_manual_mirror\vulkan_offline_mirror`

Verify configuration using the tools:
- Run `vulkan_quick_reference` to see the resolved `Registry source` path.
- Run `vulkan_health` to view resolved paths and DB status (entity counts, ok flag).

## Claude Desktop Configuration (Windows)

Integrate the MCP server into Claude Desktop on Windows. The server autodetects the sibling registry under `vulkan_docs\xml\vk.xml`, so env vars are optional. Adding them makes paths explicit and portable.

- Server script path: `E:\coding_projects\ue_manual_mirror\vulkan_offline_mirror\mcp_server\vulkan_mcp_fixed_final.py`
- Recommended env:
  - `VULKAN_DOCS_ROOT=E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror`
  - or `VULKAN_REGISTRY_XML=E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror\\vulkan_docs\\xml\\vk.xml`
  - optional `VULKAN_MCP_DB=E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror\\mcp_server\\vulkan_docs.db`

Example `mcpServers` block:

```
{
  "mcpServers": {
    "enhanced-vulkan-docs": {
      "command": "python",
      "args": [
        "E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror\\mcp_server\\vulkan_mcp_fixed_final.py"
      ],
      "env": {
        "PYTHONUNBUFFERED": "1",
        "PYTHONIOENCODING": "utf-8",
        "VULKAN_DOCS_ROOT": "E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror",
        "VULKAN_MCP_DB": "E:\\coding_projects\\ue_manual_mirror\\vulkan_offline_mirror\\mcp_server\\vulkan_docs.db"
      }
    }
  }
}
```

After starting Claude Desktop:
- Run `tools/list` to ensure `search_vulkan_api`, `get_vulkan_entity`, `browse_vulkan_hierarchy`, `vulkan_quick_reference`, and `vulkan_health` are available.
- Run `vulkan_health` to confirm `ok: True` and the resolved `registry_path`.
- Try a search: `search_vulkan_api("vkCreate", entity_type="function", limit=5)`.

## Available Tools

### 1. `search_vulkan_api`
**Purpose:** General searching across all Vulkan entities  
**Parameters:**
- `query` (required): Search term
- `entity_type`: Filter by type (`all`, `function`, `structure`, `extension`)
- `limit`: Max results (default 10)

**Search Behavior:**
- Case-insensitive SQL LIKE matching
- Searches in: name, description, AND keywords
- Priority order:
  1. Exact name matches (highest)
  2. Name starts with query
  3. Name contains query
  4. Description/keywords contain query

**Best Practices:**
```
✅ GOOD searches:
- "vkCreate" - finds all creation functions
- "buffer" - finds buffer-related entities
- "compute" - finds compute shader functions
- "VK_KHR" - finds Khronos extensions

❌ AVOID:
- Spaces in function names: "vk Create" won't match "vkCreate"
- Wildcards: SQL % or * characters aren't needed
- Over-specific: "vkCreateBufferForVertexData" won't match anything
```

**Quirks:**
- Returns maximum 10 results by default (increase limit if needed)
- Partial matching works but may return unexpected results
- Searching "2" finds all Vulkan 1.2+ functions (vkQueueSubmit2, etc.)

### 2. `get_vulkan_entity`
**Purpose:** Get exact details for a specific entity  
**Parameters:**
- `name` (required): Exact entity name

**Best Practices:**
```
✅ CORRECT usage:
- "vkCreateBuffer"
- "VK_KHR_swapchain"
- "VkBufferCreateInfo"

❌ WON'T WORK:
- Partial names: "CreateBuff"
- With spaces: "vk Create Buffer"
- Wrong case won't matter (it's case-insensitive)
```

**Quirks:**
- Must be exact name (but case-insensitive)
- Returns null if not found exactly
- Good for when you know the exact API name

### 3. `browse_vulkan_hierarchy`
**Purpose:** Browse categories or list entities by type  
**Parameters:**
- `browse_type`: `all_types`, `functions`, `structures`, `extensions`

**What it returns:**
- `all_types`: Count of each entity type
- `functions`: First 50 functions alphabetically
- `structures`: First 50 structures/handles
- `extensions`: ALL extensions (not limited)

**Quirks:**
- Functions/structures limited to 50 entries
- Extensions shows all 637 (not limited)
- "structures" browse actually searches for: structure, handle, struct types

### 4. `vulkan_quick_reference`
**Purpose:** Database statistics and overview  
**Parameters:** None

**Returns:**
- Total entity count
- Breakdown by type
- Core function count (excluding extensions)
- Sample modern functions
- Database file paths

### 5. `vulkan_health`
**Purpose:** Configuration/health check for troubleshooting  
**Parameters:** None

**Returns:**
- Platform and WSL detection
- Resolved `registry_path` and whether it exists
- Resolved `database_path` and whether it exists
- Table presence and entity/function counts
- `ok` boolean for quick status

## Entity Type Breakdown

Your database contains these entity types:
- **enum_value** (1588): Individual enum constants like `VK_SUCCESS`
- **function** (675): API functions like `vkCreateBuffer`
- **extension** (637): Extensions like `VK_KHR_swapchain`
- **bitmask** (209): Flag types like `VkBufferUsageFlags`
- **handle** (58): Opaque handles like `VkDevice`, `VkBuffer`
- **define** (18): Preprocessor defines
- **funcpointer** (11): Function pointer types for callbacks
- **basetype** (7): Fundamental types

## Search Strategies for Common Tasks

### Finding Compute Shader Functions
```
Search: "compute"
Better: "dispatch" (for vkCmdDispatch variants)
Best: "vkCmdDispatch" then browse related
```

### Finding Memory Management
```
Search: "memory"
Better: "allocate" or "vkAllocate"
For barriers: "barrier"
```

### Finding Buffer Operations
```
Search: "buffer"
For creation: "vkCreateBuffer"
For copying: "vkCmdCopyBuffer"
```

### Finding Modern Vulkan 1.3/1.4 Features
```
Search: "2" (finds vkQueueSubmit2, etc.)
Search: "rendering" (finds dynamic rendering)
Search: "sync" (finds synchronization2 features)
```

### Finding Extensions
```
By vendor:
- "KHR" - Khronos ratified
- "EXT" - Multi-vendor
- "NV" - NVIDIA specific
- "AMD" - AMD specific

By feature:
- "ray_tracing"
- "mesh_shader"
- "swapchain"
```

## Known Limitations

1. **Structure Categorization**: Some structures may be categorized as 'handle' or other types instead of 'structure'
   - Example: VkRenderingInfo might be under 'handle' not 'structure'
   
2. **Search Result Limit**: Default 10 results may hide relevant entries
   - Solution: Increase limit parameter when searching broad terms

3. **No Regex Support**: Uses SQL LIKE, not regular expressions
   - Can't use complex patterns
   - Simple wildcards handled automatically

4. **No Parameter Details**: Function descriptions show signatures but not detailed parameter docs
   - Shows: `VkResult vkCreateBuffer(VkDevice device, ...)`
   - Doesn't show: Parameter descriptions, valid usage, etc.

5. **No Version Filtering**: Can't filter by Vulkan version (1.0, 1.1, etc.)
   - Version info stored but not searchable

6. **Path Resolution Across Environments**: If `vk.xml` can’t be located, search/get won’t return real data
   - Fix: Set `VULKAN_REGISTRY_XML` or `VULKAN_DOCS_ROOT`, or rely on the repo mirror
   - Use `vulkan_health` to confirm resolved paths and DB status

## Tips for Fluid Dynamics Development

For your fluid dynamics simulation, key searches:

1. **Compute Pipeline**: 
   - `get_vulkan_entity("vkCreateComputePipelines")`
   - `search_vulkan_api("dispatch")`

2. **Buffer Management**:
   - `search_vulkan_api("storage", entity_type="function")`
   - `get_vulkan_entity("vkCreateBuffer")`

3. **Synchronization**:
   - `search_vulkan_api("barrier")`
   - `search_vulkan_api("fence")`
   - `get_vulkan_entity("vkCmdPipelineBarrier2")` (modern)

4. **Memory Operations**:
   - `search_vulkan_api("memory", entity_type="function")`
   - `get_vulkan_entity("vkMapMemory")`

## Quick Command Reference

Ask Claude:
- "Search Vulkan for compute functions"
- "Get details on vkCmdDispatch"
- "Browse Vulkan extensions"
- "Show Vulkan database stats"
- "Find buffer-related Vulkan functions"
- "List modern Vulkan synchronization features"

## Troubleshooting

**No results found?**
- Check spelling (vkCreateBuffer not vkCreateBufer)
- Try broader terms ("buffer" instead of "vertex buffer")
- Remove prefixes ("Pipeline" instead of "vkCmdBindPipeline")

**Too many results?**
- Use entity_type filter
- Be more specific in query
- Use get_vulkan_entity for exact matches

**Missing expected functions?**
- Some may be under extensions (search extension name)
- Try searching without "vk" prefix
- Check if it's a typedef or macro (under 'define' type)

## Database Statistics
- **Total Entities**: 3203
- **Coverage**: Complete Vulkan 1.4.321.1
- **Most Populated**: Enum values (1588)
- **Functions**: 675 (all core + extensions)
- **Best for**: Quick API reference, not deep documentation
