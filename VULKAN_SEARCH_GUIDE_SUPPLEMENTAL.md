# Claude Code Vulkan MCP Server - Efficient Usage Guide
*Supplement to VULKAN_SEARCH_GUIDE.md*

## 🎯 Quick Start for Claude Code

When using this server with Claude Code for Vulkan 1.4 development, follow these patterns for maximum efficiency:

## 📋 Essential Usage Patterns

### 1. Start with Concepts, Not Exact Names
```
❌ BAD: "vkCmdBeginRenderingKHR"  
✅ GOOD: "dynamic rendering"

❌ BAD: "VkPhysicalDeviceVulkan13Features"
✅ GOOD: "vulkan 1.3 features"
```

### 2. Use Synonyms - The Server Understands Related Terms
The enhanced search automatically expands:
- `memory` → allocation, heap, buffer, allocate
- `sync` → synchronization, barrier, semaphore, fence  
- `render` → rendering, draw, graphics, pipeline
- `compute` → dispatch, shader, workgroup
- `buffer` → memory, storage, uniform, vertex

### 3. Efficient Query Strategies

#### For Implementation Questions:
```
1. search_vulkan_spec("concept") - Get detailed docs
2. search_vulkan_api("vkFunction") - Get exact signatures
3. get_function_spec("vkFunction") - Get all spec references
```

#### For Modern Vulkan (1.3/1.4):
```
- search_vulkan_spec("dynamic rendering") 
- search_vulkan_spec("synchronization2")
- search_vulkan_spec("timeline semaphore")
```

## 🚀 Claude Code Prompting Tips

### Tell Claude Code to Use Both Tools:
"Use the Vulkan MCP server to find the spec requirements for [feature], then show me the implementation"

### For Debugging:
"Search the Vulkan spec for validation rules about [error topic]"

### For Modern Features:
"Find Vulkan 1.3/1.4 alternatives to [old feature] using the spec search"

## ⚡ Performance Tips

### 1. Broad → Specific
Start with concept searches, then drill down:
```
search_vulkan_spec("compute shader") → 5 results
get_spec_section("34.1") → specific section
```

### 2. Use Keywords, Not Sentences
```
❌ "how do I allocate memory in Vulkan"
✅ "memory allocation"
```

### 3. Leverage Auto-Expansion
Single words often work best:
```
"pipeline" finds: pipeline, render, graphics, compute
"memory" finds: allocation, heap, buffer, device memory
```

## 🎮 For Your Fluid Dynamics Project

### Compute Pipeline Queries:
- `search_vulkan_spec("compute dispatch")`
- `search_vulkan_spec("storage buffer")`
- `search_vulkan_spec("workgroup")`

### Memory Management:
- `search_vulkan_spec("buffer device address")`
- `search_vulkan_spec("memory allocation")`

### Synchronization:
- `search_vulkan_spec("pipeline barrier")`
- `search_vulkan_spec("memory barrier")`
- `get_vulkan_entity("vkCmdPipelineBarrier2")`

## 📊 Token-Efficient Workflows

### Minimal Queries, Maximum Info:
1. **One concept search** → Multiple relevant sections
2. **One API search** with limit=20 → Batch of related functions
3. **One spec section** → Complete implementation requirements

### Avoid:
- Multiple similar searches
- Searching for exact function names in spec (use get_function_spec instead)
- Over-specific queries that return no results

## 🔍 Troubleshooting Search

### No Results?
1. Try broader terms: "buffer" instead of "vertex buffer object"
2. Remove prefixes: "rendering" instead of "vkCmdBeginRendering"
3. Try synonyms: "sync" instead of "synchronization"

### Too Many Results?
1. Add context: "compute memory" instead of just "memory"
2. Use section_filter: `section_filter="11"` for memory chapters
3. Reduce max_results: Default is 5, often sufficient

## 💡 Claude Code Integration Examples

### Ask Claude Code:
```
"Using the Vulkan MCP server, find the modern Vulkan 1.3 way to do render passes, 
then create a simple example"

"Search the spec for compute shader local memory requirements, 
then help me optimize my fluid simulation kernel"

"What does the Vulkan spec say about vkCmdDispatch limitations? 
Check both the API reference and specification"
```

## 🚦 Quick Reference

| Task | Best Tool | Example |
|------|-----------|---------|
| Find function signature | `get_vulkan_entity` | `("vkCreateBuffer")` |
| Understand concept | `search_vulkan_spec` | `("memory allocation")` |
| List all extensions | `browse_vulkan_hierarchy` | `("extensions")` |
| Find function requirements | `get_function_spec` | `("vkQueueSubmit2")` |
| Check modern features | `search_vulkan_spec` | `("vulkan 1.3")` |

## 📈 Success Metrics

You'll know you're using it efficiently when:
- ✅ Claude Code finds answers in 1-2 tool calls
- ✅ Search returns relevant results in top 3
- ✅ You get spec + API info together
- ✅ Modern Vulkan features are prioritized

---
*Remember: The server already has the complete Vulkan 1.4 spec indexed. Let it do the heavy lifting so Claude Code can focus on helping you write great graphics code!*
