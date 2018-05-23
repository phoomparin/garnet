// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/client/symbols/module_symbol_index.h"

#include <limits>

#include "garnet/bin/zxdb/client/symbols/dwarf_die_decoder.h"
#include "garnet/bin/zxdb/client/symbols/module_symbol_index_node.h"
#include "garnet/public/lib/fxl/logging.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"

namespace zxdb {

// Note: interesting file name extraction in dumpAttribute implementation in
// DWARFDIE.cpp.

namespace {

// We want to index the things that can have software breakpoints attached to
// them. These are the DW_TAG_subprogram entries that have a range of code.
// These implementations won't always have the full type information, when the
// declaration is separate from the implementation, the implementation will
// reference the separate declaration node. The declaration of the function
// will contain the name and have the proper nesting inside classes and
// namespaces, etc. according to the structure of the original code.
//
// In a compile unit (basically one object file), there will likely be lots of
// declarations from all the headers, and a smaller number of actual function
// definitions.
//
// From a high level, we want to search the DIEs for subprogram implementations,
// Then follow the link to their definition (if separate from the
// implementation), then walk up the tree to get the full class and namespacing
// information. But walking the tree upwards requires lots of linear searching
// since the tree is stored in a flat array.
//
// To index efficiently, do two passes:
//  1. Walk linearly through all DIEs:
//     1a. Find the ones we're interested in and save the information.
//     1b. For each one, save the index of the parent so we can efficiently
//         walk up the tree in pass 2.
//  2. Resolve the full type information for each function:
//     2a. Find the declaration for each function implementation DIE.
//     2b. Walk that declaration up to get the full context.
//     2c. Index that.
//
// Performance note: Having the unit extract its DIEs via DWARFUnit::dies() and
// DWARFUnit::getNumDIEs() basically iterates through the whole table, which
// we then do again here. We can probably speed things up by eliminating this
// call, calling unit.getUnitDIE(), and manually iterating the children of
// that.

// Stores the information from a function DIE that has code, representing
// something we want to index. The entry will always refer to the DIE for the
// implementation, and the offset will refer to the offset of the DIE for the
// definition.
//
// Some functions have separate definitions, and some don't If the definition
// and implementation is the same, the offset will just point to the entry.
// This makes slightly more work, but this simplified the control flow and
// we're optimizing for most functions having definitions.
struct FunctionImpl {
  FunctionImpl(const llvm::DWARFDebugInfoEntry* e, uint64_t offs)
      : entry(e), definition_unit_offset(offs) {}

  const llvm::DWARFDebugInfoEntry* entry;
  uint64_t definition_unit_offset;
};

// Index used to indicate there is no parent.
constexpr unsigned kNoParent = std::numeric_limits<unsigned>::max();

// Step 1 of the algorithm above. Fills the function_impls array with the
// information for all function implementations (ones with addresses). Fills
// the parent_indices array with the index of the parent of each DIE in the
// unit (it will be exactly unit->getNumDIEs() long). The root node will have
// kNoParent set.
void ExtractUnitFunctionImplsAndParents(
    llvm::DWARFContext* context,
    llvm::DWARFCompileUnit* unit,
    std::vector<FunctionImpl>* function_impls,
    std::vector<unsigned>* parent_indices) {
  DwarfDieDecoder decoder(context, unit);

  // The offset of the declaration. This can be unit-relative or file-absolute.
  // This code doesn't implement the file-absolute variant which it seems our
  // toolchain doesn't generate. To implement I'm thinking everything
  // with an absolute offset will be put into a global list and processed in a
  // third pass once all units are processed. This third pass will be slower
  // since probably we won't do any optimized lookups.
  llvm::Optional<uint64_t> decl_unit_offset;
  llvm::Optional<uint64_t> decl_global_offset;
  decoder.AddReference(llvm::dwarf::DW_AT_specification,
                       &decl_unit_offset, &decl_global_offset);

  // This flag will be set for any function that has code.
  bool has_code = false;
  decoder.AddPresenceCheck(llvm::dwarf::DW_AT_low_pc, &has_code);
  decoder.AddPresenceCheck(llvm::dwarf::DW_AT_high_pc, &has_code);
  decoder.AddPresenceCheck(llvm::dwarf::DW_AT_ranges, &has_code);

  // Stores the index of the parent DIE for each one we encounter. The root
  // DIE with no parent will be set to kNoParent.
  unsigned die_count = unit->getNumDIEs();
  parent_indices->resize(die_count);

  // Stores the list of parent indices according to the current depth in the
  // tree. At any given point, the parent index of the current node will be
  // tree_stack.back().
  struct StackEntry {
    StackEntry(int d, unsigned i) : depth(d), index(i) {}

    int depth;
    unsigned index;
  };
  std::vector<StackEntry> tree_stack;
  tree_stack.reserve(8);
  tree_stack.push_back(StackEntry(-1, kNoParent));

  for (unsigned i = 0; i < die_count; i++) {
    // All optional variables need to be reset so we know which ones are set
    // by the current DIE.
    decl_unit_offset.reset();
    decl_global_offset.reset();
    has_code = false;

    // Decode.
    const llvm::DWARFDebugInfoEntry* die =
        unit->getDIEAtIndex(i).getDebugInfoEntry();
    if (decoder.Decode(*die) &&
        die->getAbbreviationDeclarationPtr()->getTag() ==
            llvm::dwarf::DW_TAG_subprogram &&
        has_code) {
      // Found a function implementation.
      if (decl_unit_offset) {
        // Save the declaration for indexing.
        function_impls->emplace_back(die, *decl_unit_offset);
      } else if (decl_global_offset) {
        FXL_NOTREACHED() << "Implement DW_FORM_ref_addr for references.";
      } else {
        // This function has no separate definition so use it as its own
        // declaration (the name and such will be on itself).
        function_impls->emplace_back(die, die->getOffset());
      }
    }

    StackEntry& tree_stack_back = tree_stack.back();
    int current_depth = static_cast<int>(die->getDepth());
    if (current_depth == tree_stack_back.depth) {
      // Common case: depth not changing. Just update the topmost item in the
      // stack to point to the current node.
      tree_stack_back.index = i;
    } else {
      // Tree changed. First check for moving up in the tree and pop the stack
      // until we're at the parent of the current level (for going deeper in
      // the tree this will do nothing), then add the current level.
      while (tree_stack.back().depth >= current_depth)
        tree_stack.pop_back();
      tree_stack.push_back(StackEntry(current_depth, i));
    }

    // Save parent info. The parent of this node is the one right before the
    // current one (the last one in the stack).
    (*parent_indices)[i] = (tree_stack.end() - 2)->index;
  }
}

// The per-function part of step 2 of the algorithm described above. This
// finds the definition of the function in the unit's DIEs. It's given a
// map of DIE indices to their parent indices generated for the unit by
// ExtractUnitFunctionImplsAndParents for quickly finding parents.
class FunctionImplIndexer {
 public:
  FunctionImplIndexer(llvm::DWARFContext* context,
                      llvm::DWARFCompileUnit* unit,
                      const std::vector<unsigned>& parent_indices,
                      ModuleSymbolIndexNode* root)
      : unit_(unit), parent_indices_(parent_indices), root_(root),
        decoder_(context, unit) {
    decoder_.AddCString(llvm::dwarf::DW_AT_name, &name_);
  }

  void AddFunction(const FunctionImpl& impl) {
    // Components of the function name in reverse order (so "foo::Bar::Fn")
    // would be { "Fn", "Bar", "foo"}
    std::vector<std::string> components;

    // Find the declaration DIE for the function. Perf note: getDIEForOffset()
    // is a binary search.
    llvm::DWARFDie die = unit_->getDIEForOffset(impl.definition_unit_offset);
    if (!die.isValid())
      return;  // Invalid
    if (die.getTag() != llvm::dwarf::DW_TAG_subprogram)
      return;  // Not a function.
    if (!FillName(die))
      return;
    components.emplace_back(*name_);

    unsigned index = unit_->getDIEIndex(die);
    while (true) {
      // Move up one level in the hierarchy.
      FXL_DCHECK(index <= parent_indices_.size());
      index = parent_indices_[index];
      if (index == kNoParent) {
        // Reached the root. In practice this shouldn't happen since following
        // the parent chain from a function should always lead to the compile
        // unit (handled below).
        break;
      }

      die = unit_->getDIEAtIndex(index);
      if (!die.isValid())
        return;  // Something is corrupted, don't add this function.

      if (die.getTag() == llvm::dwarf::DW_TAG_compile_unit)
        break;  // Reached the root.

      // Validate the type of this entry. We don't want to index things
      // like functions inside classes locally defined in functions since
      // there's no good way to refer to these by global name.
      if (die.getTag() != llvm::dwarf::DW_TAG_namespace &&
          die.getTag() != llvm::dwarf::DW_TAG_class_type &&
          die.getTag() != llvm::dwarf::DW_TAG_structure_type)
        return;

      if (!FillName(die))
        return;  // Likely corrupt, these nodes should have names.
      components.emplace_back(*name_);
    }

    // Add the function to the index.
    ModuleSymbolIndexNode* cur = root_;
    for (int i = static_cast<int>(components.size()) - 1; i >= 0; i--)
      cur = cur->AddChild(std::move(components[i]));
    cur->AddFunctionDie(llvm::DWARFDie(unit_, impl.entry));
  }

 private:
  // Fills the name_ member for the given DIE. Returns true if the DIE was
  // decoded properly and name_ was properly filled in.
  bool FillName(const llvm::DWARFDie& die) {
    name_.reset();
    if (!decoder_.Decode(*die.getDebugInfoEntry()) || !name_)
      return false;  // Node with no name, skip this function.
    return true;
  }

  llvm::DWARFCompileUnit* unit_;
  const std::vector<unsigned>& parent_indices_;
  ModuleSymbolIndexNode* root_;

  DwarfDieDecoder decoder_;
  llvm::Optional<const char*> name_;  // Decoder writes into this.
};

}  // namespace

ModuleSymbolIndex::ModuleSymbolIndex() = default;
ModuleSymbolIndex::~ModuleSymbolIndex() = default;

void ModuleSymbolIndex::CreateIndex(
    llvm::DWARFContext* context,
    llvm::DWARFUnitSection<llvm::DWARFCompileUnit>& units) {
  for (const std::unique_ptr<llvm::DWARFCompileUnit>& unit : units)
    IndexCompileUnit(context, unit.get());
}

const std::vector<llvm::DWARFDie>& ModuleSymbolIndex::FindFunctionExact(
    const std::string& input) const {
  // Split the input on "::" which we'll traverse the tree with.
  //
  // TODO(brettw) this doesn't handle a lot of things, like handle templates.
  // By blindly splitting on "::" we'll never find functions like
  // "std::vector<Foo::Bar>::insert".
  std::string separator("::");

  const ModuleSymbolIndexNode* cur = &root_;

  size_t input_index = 0;
  while (input_index < input.size()) {
    size_t next = input.find(separator, input_index);

    std::string cur_name;
    if (next == std::string::npos) {
      cur_name = input.substr(input_index);
      input_index = input.size();
    } else {
      cur_name = input.substr(input_index, next - input_index);
      input_index = next + separator.size();  // Skip over "::".
    }

    auto found = cur->sub().find(cur_name);
    if (found == cur->sub().end()) {
      static std::vector<llvm::DWARFDie> empty_vector;
      return empty_vector;
    }

    cur = &found->second;
  }

  return cur->function_dies();
}

void ModuleSymbolIndex::IndexCompileUnit(llvm::DWARFContext* context,
                                         llvm::DWARFCompileUnit* unit) {
  // Find the things to index.
  std::vector<FunctionImpl> function_impls;
  function_impls.reserve(256);
  std::vector<unsigned> parent_indices;
  ExtractUnitFunctionImplsAndParents(
      context, unit, &function_impls, &parent_indices);

  // Index each one.
  FunctionImplIndexer indexer(context, unit, parent_indices, &root_);
  for (const FunctionImpl& impl : function_impls)
    indexer.AddFunction(impl);
}

}  // namespace zxdb