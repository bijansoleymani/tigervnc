/* Copyright 2016 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <math.h>

#include <stdexcept>

#include <ApplicationServices/ApplicationServices.h>

#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>

#include "cocoa.h"
#include "Surface.h"

static CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

static CGImageRef create_image(CGColorSpaceRef lut,
                               const unsigned char* data,
                               int w, int h, bool skip_alpha)
{
  CGDataProviderRef provider;
  CGImageAlphaInfo alpha;

  CGImageRef image;

  provider = CGDataProviderCreateWithData(nullptr, data,
                                          w * h * 4, nullptr);
  if (!provider)
    throw std::runtime_error("CGDataProviderCreateWithData");

  // FIXME: This causes a performance hit, but is necessary to avoid
  //        artifacts in the edges of the window
  if (skip_alpha)
    alpha = kCGImageAlphaNoneSkipFirst;
  else
    alpha = kCGImageAlphaPremultipliedFirst;

  image = CGImageCreate(w, h, 8, 32, w * 4, lut,
                        alpha | kCGBitmapByteOrder32Little,
                        provider, nullptr, false,
                        kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  if (!image)
    throw std::runtime_error("CGImageCreate");

  return image;
}

static void render(CGContextRef gc, CGColorSpaceRef lut,
                   const unsigned char* data,
                   CGBlendMode mode, CGFloat alpha,
                   int img_w, int img_h,
                   int src_x, int src_y, int src_w, int src_h,
                   CGFloat dst_x, CGFloat dst_y, CGFloat dst_w, CGFloat dst_h)
{
  CGRect rect;
  CGImageRef image, subimage;

  image = create_image(lut, data, img_w, img_h, mode == kCGBlendModeCopy);

  // Crop the requested source region (in image/source pixels)
  rect.origin.x = src_x;
  rect.origin.y = src_y;
  rect.size.width = src_w;
  rect.size.height = src_h;

  subimage = CGImageCreateWithImageInRect(image, rect);
  if (!subimage)
    throw std::runtime_error("CGImageCreateImageWithImageInRect");

  CGContextSaveGState(gc);

  CGContextSetBlendMode(gc, mode);
  CGContextSetAlpha(gc, alpha);

  // Destination rectangle (in device pixels); may differ in size from the
  // source region on HiDPI/Retina displays, in which case the image is
  // scaled up to fill it.
  rect.origin.x = dst_x;
  rect.origin.y = dst_y;
  rect.size.width = dst_w;
  rect.size.height = dst_h;

  CGContextDrawImage(gc, rect, subimage);

  CGContextRestoreGState(gc);

  CGImageRelease(subimage);
  CGImageRelease(image);
}

static CGContextRef make_bitmap(int width, int height, unsigned char* data)
{
  CGContextRef bitmap;

  bitmap = CGBitmapContextCreate(data, width, height, 8, width*4, srgb,
                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  if (!bitmap)
    throw std::runtime_error("CGBitmapContextCreate");

  return bitmap;
}

void Surface::clear(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
  unsigned char* out;
  int x, y;

  r = (unsigned)r * a / 255;
  g = (unsigned)g * a / 255;
  b = (unsigned)b * a / 255;

  out = data;
  for (y = 0;y < width();y++) {
    for (x = 0;x < height();x++) {
      *out++ = b;
      *out++ = g;
      *out++ = r;
      *out++ = a;
    }
  }
}

void Surface::draw(int src_x, int src_y, int dst_x, int dst_y,
                   int dst_w, int dst_h)
{
  CGColorSpaceRef lut;
  CGAffineTransform ctm;
  CGFloat sx, sy;
  CGFloat dev_x, dev_y;

  CGContextSaveGState(fl_gc);

  // The context's current transform maps points to device pixels. On
  // HiDPI/Retina displays this scale is >1, so capture it before we reset
  // the matrix, otherwise we'd draw at 1:1 pixel scale and only fill a
  // fraction of the (larger) backing store.
  ctm = CGContextGetCTM(fl_gc);
  sx = fabs(ctm.a);
  sy = fabs(ctm.d);

  // Reset the transformation matrix back to the default identity
  // matrix as otherwise we get a massive performance hit
  CGContextConcatCTM(fl_gc, CGAffineTransformInvert(ctm));

  // macOS Coordinates are from bottom left, not top left. Compute the
  // destination in device pixels using the captured scale factors.
  dev_x = dst_x * sx;
  dev_y = (Fl_Window::current()->h() - (dst_y + dst_h)) * sy;

  lut = cocoa_win_color_space(Fl_Window::current());
  render(fl_gc, lut, data, kCGBlendModeCopy, 1.0,
         width(), height(), src_x, src_y, dst_w, dst_h,
         dev_x, dev_y, dst_w * sx, dst_h * sy);
  CGColorSpaceRelease(lut);

  CGContextRestoreGState(fl_gc);
}

void Surface::draw(Surface* dst, int src_x, int src_y,
                   int dst_x, int dst_y, int dst_w, int dst_h)
{
  CGContextRef bitmap;

  bitmap = make_bitmap(dst->width(), dst->height(), dst->data);

  // macOS Coordinates are from bottom left, not top left
  dst_y = dst->height() - (dst_y + dst_h);

  // Drawing into an offscreen bitmap, so no HiDPI scaling involved
  render(bitmap, srgb, data, kCGBlendModeCopy, 1.0,
         width(), height(), src_x, src_y, dst_w, dst_h,
         dst_x, dst_y, dst_w, dst_h);

  CGContextRelease(bitmap);
}

void Surface::blend(int src_x, int src_y, int dst_x, int dst_y,
                    int dst_w, int dst_h, int a)
{
  CGColorSpaceRef lut;
  CGAffineTransform ctm;
  CGFloat sx, sy;
  CGFloat dev_x, dev_y;

  CGContextSaveGState(fl_gc);

  // Capture the points-to-device-pixels scale before resetting the matrix
  // (see Surface::draw() for details)
  ctm = CGContextGetCTM(fl_gc);
  sx = fabs(ctm.a);
  sy = fabs(ctm.d);

  // Reset the transformation matrix back to the default identity
  // matrix as otherwise we get a massive performance hit
  CGContextConcatCTM(fl_gc, CGAffineTransformInvert(ctm));

  // macOS Coordinates are from bottom left, not top left
  dev_x = dst_x * sx;
  dev_y = (Fl_Window::current()->h() - (dst_y + dst_h)) * sy;

  lut = cocoa_win_color_space(Fl_Window::current());
  render(fl_gc, lut, data, kCGBlendModeNormal, (CGFloat)a/255.0,
         width(), height(), src_x, src_y, dst_w, dst_h,
         dev_x, dev_y, dst_w * sx, dst_h * sy);
  CGColorSpaceRelease(lut);

  CGContextRestoreGState(fl_gc);
}

void Surface::blend(Surface* dst, int src_x, int src_y,
                    int dst_x, int dst_y, int dst_w, int dst_h, int a)
{
  CGContextRef bitmap;

  bitmap = make_bitmap(dst->width(), dst->height(), dst->data);

  // macOS Coordinates are from bottom left, not top left
  dst_y = dst->height() - (dst_y + dst_h);

  // Drawing into an offscreen bitmap, so no HiDPI scaling involved
  render(bitmap, srgb, data, kCGBlendModeNormal, (CGFloat)a/255.0,
         width(), height(), src_x, src_y, dst_w, dst_h,
         dst_x, dst_y, dst_w, dst_h);

  CGContextRelease(bitmap);
}

void Surface::alloc()
{
  data = new unsigned char[width() * height() * 4];
}

void Surface::dealloc()
{
  delete [] data;
}

void Surface::update(const Fl_RGB_Image* image)
{
  int x, y;
  const unsigned char* in;
  unsigned char* out;

  assert(image->w() == width());
  assert(image->h() == height());

  // Convert data and pre-multiply alpha
  in = (const unsigned char*)image->data()[0];
  out = data;
  for (y = 0;y < image->h();y++) {
    for (x = 0;x < image->w();x++) {
      switch (image->d()) {
      case 1:
        *out++ = in[0];
        *out++ = in[0];
        *out++ = in[0];
        *out++ = 0xff;
        break;
      case 2:
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = in[1];
        break;
      case 3:
        *out++ = in[2];
        *out++ = in[1];
        *out++ = in[0];
        *out++ = 0xff;
        break;
      case 4:
        *out++ = (unsigned)in[2] * in[3] / 255;
        *out++ = (unsigned)in[1] * in[3] / 255;
        *out++ = (unsigned)in[0] * in[3] / 255;
        *out++ = in[3];
        break;
      }
      in += image->d();
    }
    if (image->ld() != 0)
      in += image->ld() - image->w() * image->d();
  }
}
