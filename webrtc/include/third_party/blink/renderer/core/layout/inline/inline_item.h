// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_INLINE_INLINE_ITEM_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_INLINE_INLINE_ITEM_H_

#include <unicode/ubidi.h>

#include "base/check_op.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/layout/inline/inline_item_segment.h"
#include "third_party/blink/renderer/core/layout/inline/text_item_type.h"
#include "third_party/blink/renderer/core/layout/layout_inline.h"
#include "third_party/blink/renderer/core/layout/style_variant.h"
#include "third_party/blink/renderer/platform/fonts/shaping/run_segmenter.h"
#include "third_party/blink/renderer/platform/text/text_direction.h"

namespace blink {

class LayoutObject;

// Class representing a single text node or styled inline element with text
// content segmented by style, text direction, sideways rotation, font fallback
// priority (text, symbol, emoji, etc), and script (but not by font).
// In this representation TextNodes are merged up into their parent inline
// element where possible.
class CORE_EXPORT InlineItem {
  DISALLOW_NEW();

 public:
  enum InlineItemType {
    kText,
    kControl,
    kAtomicInline,
    kBlockInInline,
    kOpenTag,
    kCloseTag,
    kFloating,
    kOutOfFlowPositioned,
    kInitialLetterBox,
    kListMarker,
    kBidiControl
  };

  enum CollapseType {
    // No collapsible spaces.
    kNotCollapsible,
    // This item is opaque to whitespace collapsing.
    kOpaqueToCollapsing,
    // This item ends with collapsible spaces.
    kCollapsible,
    // Collapsible spaces at the end of this item were collapsed.
    kCollapsed,
  };

  // The constructor and destructor can't be implicit or inlined, because they
  // require full definition of ComputedStyle.
  InlineItem(InlineItemType type,
             unsigned start,
             unsigned end,
             LayoutObject* layout_object);
  ~InlineItem();

  // Copy constructor adjusting start/end and shape results.
  InlineItem(const InlineItem&,
             unsigned adjusted_start,
             unsigned adjusted_end,
             scoped_refptr<const ShapeResult>);

  InlineItemType Type() const { return type_; }
  const char* InlineItemTypeToString(InlineItemType val) const;

  TextItemType TextType() const {
    return static_cast<TextItemType>(text_type_);
  }
  bool IsForcedLineBreak() const {
    return TextType() == TextItemType::kForcedLineBreak;
  }
  void SetTextType(TextItemType text_type) {
    text_type_ = static_cast<unsigned>(text_type);
  }
  bool IsSymbolMarker() const {
    return TextType() == TextItemType::kSymbolMarker;
  }
  void SetIsSymbolMarker() {
    DCHECK(TextType() == TextItemType::kNormal ||
           TextType() == TextItemType::kSymbolMarker);
    SetTextType(TextItemType::kSymbolMarker);
  }

  const ShapeResult* TextShapeResult() const { return shape_result_.get(); }
  ShapeResult* CloneTextShapeResult() {
    scoped_refptr<ShapeResult> clone = ShapeResult::Create(*shape_result_);
    shape_result_ = clone;
    return clone.get();
  }
  bool IsUnsafeToReuseShapeResult() const {
    return is_unsafe_to_reuse_shape_result_;
  }
  void SetUnsafeToReuseShapeResult() {
    is_unsafe_to_reuse_shape_result_ = true;
  }
#if DCHECK_IS_ON()
  void CheckTextType(const String& text_content) const;
#endif

  // If this item is "empty" for the purpose of empty block calculation.
  // Note: for block-in-inlines, this can't be determined until this is laid
  // out. This function always return |false| for the case.
  bool IsEmptyItem() const { return is_empty_item_; }
  void SetIsEmptyItem(bool value) { is_empty_item_ = value; }

  // If this item is either a float or OOF-positioned node. If an inline
  // formatting-context *only* contains these types of nodes we consider it
  // block-level, and run the |BlockLayoutAlgorithm| instead of the
  // |InlineLayoutAlgorithm|.
  bool IsBlockLevel() const { return is_block_level_; }
  void SetIsBlockLevel(bool value) { is_block_level_ = value; }

  // If this item should create a box fragment. Box fragments can be omitted for
  // optimization if this is false.
  bool ShouldCreateBoxFragment() const {
    if (Type() == kOpenTag || Type() == kCloseTag)
      return To<LayoutInline>(layout_object_.Get())->ShouldCreateBoxFragment();
    DCHECK_EQ(Type(), kAtomicInline);
    return false;
  }
  void SetShouldCreateBoxFragment() {
    DCHECK(Type() == kOpenTag || Type() == kCloseTag);
    To<LayoutInline>(layout_object_.Get())->SetShouldCreateBoxFragment();
  }

  unsigned StartOffset() const { return start_offset_; }
  unsigned EndOffset() const { return end_offset_; }
  unsigned Length() const { return end_offset_ - start_offset_; }

  bool IsValidOffset(unsigned offset) const;

  TextDirection Direction() const { return DirectionFromLevel(BidiLevel()); }
  UBiDiLevel BidiLevel() const { return static_cast<UBiDiLevel>(bidi_level_); }
  // Resolved bidi level for the reordering algorithm. Certain items have
  // artificial bidi level for the reordering algorithm without affecting its
  // direction.
  UBiDiLevel BidiLevelForReorder() const {
    // List markers should not be reordered to protect it from being included
    // into unclosed inline boxes.
    return Type() != InlineItem::kListMarker ? BidiLevel() : 0;
  }

  LayoutObject* GetLayoutObject() const { return layout_object_.Get(); }

  bool IsImage() const {
    return GetLayoutObject() && GetLayoutObject()->IsLayoutImage();
  }
  bool IsRubyColumn() const {
    return GetLayoutObject() && GetLayoutObject()->IsRubyColumn();
  }
  bool IsTextCombine() const {
    return GetLayoutObject() && GetLayoutObject()->IsLayoutTextCombine();
  }

  void SetOffset(unsigned start, unsigned end) {
    DCHECK_GE(end, start);
    start_offset_ = start;
    end_offset_ = end;
    // Any modification to the offset will invalidate the shape result.
    shape_result_ = nullptr;
  }
  void SetEndOffset(unsigned end_offset) {
    DCHECK_GE(end_offset, start_offset_);
    end_offset_ = end_offset;
    // Any modification to the offset will invalidate the shape result.
    shape_result_ = nullptr;
  }

  void SetStyleVariant(StyleVariant style_variant) {
    style_variant_ = static_cast<unsigned>(style_variant);
  }
  StyleVariant GetStyleVariant() const {
    return static_cast<StyleVariant>(style_variant_);
  }
  const ComputedStyle* Style() const {
    // Use the |ComputedStyle| in |LayoutObject|, because not all style changes
    // re-run |CollectInlines()|.
    DCHECK(layout_object_);
    return &layout_object_->EffectiveStyle(GetStyleVariant());
  }

  // Returns a screen-size font for SVG text.
  // Returns Style()->GetFont() otherwise.
  const Font& FontWithSvgScaling() const;

  // Get or set the whitespace collapse type at the end of this item.
  CollapseType EndCollapseType() const {
    return static_cast<CollapseType>(end_collapse_type_);
  }
  void SetEndCollapseType(CollapseType type) {
    // |kText| can set any types.
    DCHECK(Type() == InlineItem::kText ||
           // |kControl| and |kBlockInInline| are always |kCollapsible|.
           ((Type() == InlineItem::kControl ||
             Type() == InlineItem::kBlockInInline) &&
            type == kCollapsible) ||
           // Other types are |kOpaqueToCollapsing|.
           type == kOpaqueToCollapsing);
    end_collapse_type_ = type;
  }
  bool IsCollapsibleSpaceOnly() const {
    return Type() == InlineItem::kText && end_collapse_type_ == kCollapsible &&
           Length() == 1u;
  }

  // True if this item was generated (not in DOM).
  // InlineItemsBuilder may generate break opportunitites to express the
  // context that are lost during the whitespace collapsing. This item is used
  // during the line breaking and layout, but is not supposed to generate
  // fragments.
  bool IsGeneratedForLineBreak() const { return is_generated_for_line_break_; }
  void SetIsGeneratedForLineBreak() { is_generated_for_line_break_ = true; }

  // Whether the end collapsible space run contains a newline.
  // Valid only when kCollapsible or kCollapsed.
  bool IsEndCollapsibleNewline() const { return is_end_collapsible_newline_; }
  void SetEndCollapseType(CollapseType type, bool is_newline) {
    SetEndCollapseType(type);
    is_end_collapsible_newline_ = is_newline;
  }

  static void Split(HeapVector<InlineItem>&, unsigned index, unsigned offset);

  // RunSegmenter properties.
  unsigned SegmentData() const { return segment_data_; }
  static void SetSegmentData(const RunSegmenter::RunSegmenterRange& range,
                             HeapVector<InlineItem>* items);

  RunSegmenter::RunSegmenterRange CreateRunSegmenterRange() const {
    // Only `kText` has the `segment_data_`, see `InlineItem::SetSegmentData`.
    DCHECK_EQ(Type(), kText);
    return InlineItemSegment::UnpackSegmentData(start_offset_, end_offset_,
                                                segment_data_);
  }

  // Whether the other item has the same RunSegmenter properties or not.
  bool EqualsRunSegment(const InlineItem& other) const {
    return segment_data_ == other.segment_data_;
  }

  void SetBidiLevel(UBiDiLevel level) {
    // Invalidate ShapeResult because it depends on the resolved direction.
    if (DirectionFromLevel(level) != DirectionFromLevel(bidi_level_))
      shape_result_ = nullptr;
    bidi_level_ = level;
  }
  static unsigned SetBidiLevel(HeapVector<InlineItem>&,
                               unsigned index,
                               unsigned end_offset,
                               UBiDiLevel);

  void AssertOffset(unsigned offset) const { DCHECK(IsValidOffset(offset)); }
  void AssertEndOffset(unsigned offset) const;

  String ToString() const;

  void Trace(Visitor* visitor) const;

 private:
  void ComputeBoxProperties();

  unsigned start_offset_;
  unsigned end_offset_;
  scoped_refptr<const ShapeResult> shape_result_;
  Member<LayoutObject> layout_object_;

  InlineItemType type_;
  unsigned text_type_ : 3;          // TextItemType
  unsigned style_variant_ : 2;      // StyleVariant
  unsigned end_collapse_type_ : 2;  // CollapseType
  unsigned bidi_level_ : 8;         // UBiDiLevel is defined as uint8_t.
  // |segment_data_| is valid only for |type_ == InlineItem::kText|.
  unsigned segment_data_ : InlineItemSegment::kSegmentDataBits;
  unsigned is_empty_item_ : 1;
  unsigned is_block_level_ : 1;
  unsigned is_end_collapsible_newline_ : 1;
  unsigned is_generated_for_line_break_ : 1;
  unsigned is_unsafe_to_reuse_shape_result_ : 1;
  friend class InlineNode;
  friend class InlineNodeDataEditor;
};

inline bool InlineItem::IsValidOffset(unsigned offset) const {
  return (offset >= start_offset_ && offset < end_offset_) ||
         (start_offset_ == end_offset_ && offset == start_offset_);
}

inline void InlineItem::AssertEndOffset(unsigned offset) const {
  DCHECK_GE(offset, start_offset_);
  DCHECK_LE(offset, end_offset_);
}

}  // namespace blink

namespace WTF {

template <>
struct VectorTraits<blink::InlineItem> : VectorTraitsBase<blink::InlineItem> {
  static constexpr bool kCanClearUnusedSlotsWithMemset = true;
  static constexpr bool kCanTraceConcurrently = true;
};

}  // namespace WTF

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_INLINE_INLINE_ITEM_H_
