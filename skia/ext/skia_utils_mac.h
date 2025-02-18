// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SKIA_EXT_SKIA_UTILS_MAC_H_
#define SKIA_EXT_SKIA_UTILS_MAC_H_

#include <ApplicationServices/ApplicationServices.h>
#include <vector>

#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPixmap.h"

struct SkIRect;
struct SkRect;
class SkCanvas;
class SkMatrix;
#ifdef __LP64__
typedef CGSize NSSize;
#else
typedef struct _NSSize NSSize;
#endif

#ifdef __OBJC__
@class NSBitmapImageRep;
@class NSImage;
@class NSImageRep;
@class NSColor;
#else
class NSBitmapImageRep;
class NSImage;
class NSImageRep;
class NSColor;
#endif

namespace skia {

// Matrix converters.
SK_API CGAffineTransform SkMatrixToCGAffineTransform(const SkMatrix& matrix);

// Rectangle converters.
SK_API SkRect CGRectToSkRect(const CGRect& rect);

// Converts a Skia rect to a CoreGraphics CGRect.
CGRect SkIRectToCGRect(const SkIRect& rect);
CGRect SkRectToCGRect(const SkRect& rect);

// Converts CGColorRef to the ARGB layout Skia expects.
SK_API SkColor CGColorRefToSkColor(CGColorRef color);

// Converts ARGB to CGColorRef.
SK_API CGColorRef CGColorCreateFromSkColor(SkColor color);

// Converts NSColor to ARGB. Returns raw rgb values and does no colorspace
// conversion. Only valid for colors in calibrated and device color spaces.
SK_API SkColor NSDeviceColorToSkColor(NSColor* color);

// Converts ARGB in the specified color space to NSColor.
// Prefer sRGB over calibrated colors.
SK_API NSColor* SkColorToCalibratedNSColor(SkColor color);
SK_API NSColor* SkColorToDeviceNSColor(SkColor color);
SK_API NSColor* SkColorToSRGBNSColor(SkColor color);

// Converts a CGImage to a SkBitmap.
SK_API SkBitmap CGImageToSkBitmap(CGImageRef image);

// Draws an NSImage with a given size into a SkBitmap.
SK_API SkBitmap NSImageToSkBitmapWithColorSpace(NSImage* image,
                                                bool is_opaque,
                                                CGColorSpaceRef color_space);

// Draws an NSImageRep with a given size into a SkBitmap.
SK_API SkBitmap NSImageRepToSkBitmapWithColorSpace(NSImageRep* image,
                                                   NSSize size,
                                                   bool is_opaque,
                                                   CGColorSpaceRef colorspace);

// Given an SkBitmap, return an autoreleased NSBitmapImageRep in the generic
// color space.
SK_API NSBitmapImageRep* SkBitmapToNSBitmapImageRep(const SkBitmap& image);

SK_API NSBitmapImageRep* SkBitmapToNSBitmapImageRepWithColorSpace(
    const SkBitmap& skiaBitmap,
    CGColorSpaceRef colorSpace);

// Given an SkBitmap and a color space, return an autoreleased NSImage.
SK_API NSImage* SkBitmapToNSImageWithColorSpace(const SkBitmap& icon,
                                                CGColorSpaceRef colorSpace);

// Given an SkBitmap, return an autoreleased NSImage in the generic color space.
// DEPRECATED, use SkBitmapToNSImageWithColorSpace() instead.
// TODO(thakis): Remove this -- http://crbug.com/69432
SK_API NSImage* SkBitmapToNSImage(const SkBitmap& icon);

// Converts a SkCanvas temporarily to a CGContext
class SK_API SkiaBitLocker {
 public:
  /**
    User clip rect is an *additional* clip to be applied in addition to the
    current state of the canvas, in *local* rather than device coordinates.
    If no additional clipping is desired, pass in
    SkIRect::MakeSize(canvas->getBaseLayerSize()) transformed by the inverse
    CTM.
   */
  SkiaBitLocker(SkCanvas* canvas,
                const SkIRect& userClipRect,
                SkScalar bitmapScaleFactor = 1);
  ~SkiaBitLocker();
  CGContextRef cgContext();
  bool hasEmptyClipRegion() const;

 private:
  void releaseIfNeeded();
  SkIRect computeDirtyRect();

  SkCanvas* canvas_;

  CGContextRef cgContext_;
  // offscreen_ is only valid if useDeviceBits_ is false
  SkBitmap offscreen_;
  SkIPoint bitmapOffset_;
  SkScalar bitmapScaleFactor_;

  // True if we are drawing to |canvas_|'s backing store directly.
  // Otherwise, the bits in |bitmap_| are our allocation and need to
  // be copied over to |canvas_|.
  bool useDeviceBits_;

  // True if |bitmap_| is a dummy 1x1 bitmap allocated for the sake of creating
  // a non-NULL CGContext (it is invalid to use a NULL CGContext), and will not
  // be copied to |canvas_|. This will happen if |canvas_|'s clip region is
  // empty.
  bool bitmapIsDummy_;
};


}  // namespace skia

#endif  // SKIA_EXT_SKIA_UTILS_MAC_H_
