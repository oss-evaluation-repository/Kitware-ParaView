/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkKWTkUtilities.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$

Copyright (c) 2000-2001 Kitware Inc. 469 Clifton Corporate Parkway,
Clifton Park, NY, 12065, USA.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name of Kitware nor the names of any contributors may be used
   to endorse or promote products derived from this software without specific 
   prior written permission.

 * Modified source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

=========================================================================*/
#include "vtkKWTkUtilities.h"

#include "vtkBase64Utility.h"
#include "vtkImageData.h"
#include "vtkImageFlip.h"
#include "vtkObjectFactory.h"

// This has to be here because on HP varargs are included in 
// tcl.h and they have different prototypes for va_start so
// the build fails. Defining HAS_STDARG prevents that.
#if defined(__hpux) && !defined(HAS_STDARG)
#  define HAS_STDARG
#endif

#include "tk.h"
#include "zlib.h"

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkKWTkUtilities);
vtkCxxRevisionMacro(vtkKWTkUtilities, "1.16");

//----------------------------------------------------------------------------
void vtkKWTkUtilities::GetRGBColor(Tcl_Interp *interp,
                                   const char *window, 
                                   const char *color, 
                                   int *rr, int *gg, int *bb)
{
  ostrstream command;
  command << "winfo rgb " << window << " " << color << ends;
  if (Tcl_GlobalEval(interp, command.str()) != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to get RGB color: " << interp->result);
    command.rdbuf()->freeze(0);     
    return;
    }
  command.rdbuf()->freeze(0);     

  int r, g, b;
  sscanf(interp->result, "%d %d %d", &r, &g, &b);
  *rr = static_cast<int>((static_cast<float>(r) / 65535.0)*255.0);
  *gg = static_cast<int>((static_cast<float>(g) / 65535.0)*255.0);
  *bb = static_cast<int>((static_cast<float>(b) / 65535.0)*255.0); 
}

//-----------------------------------------------------------------------------
void vtkKWTkUtilities::GetOptionColor(Tcl_Interp *interp,
                                      const char *window,
                                      const char *option,
                                      int *r, int *g, int *b)
{
  ostrstream command;
  command << window << " cget " << option << ends;
  if (Tcl_GlobalEval(interp, command.str()) != TCL_OK)
    {
    vtkGenericWarningMacro(
      << "Unable to get " << option << " option: " << interp->result);
    command.rdbuf()->freeze(0);     
    return;
    }
  command.rdbuf()->freeze(0);     

  vtkKWTkUtilities::GetRGBColor(interp, window, interp->result, r, g, b);
}

//-----------------------------------------------------------------------------
void vtkKWTkUtilities::GetBackgroundColor(Tcl_Interp *interp,
                                          const char *window,
                                          int *r, int *g, int *b)
{
  vtkKWTkUtilities::GetOptionColor(interp, window, "-bg", r, g, b);
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::UpdatePhoto(Tcl_Interp *interp,
                                  const char *photo_name,
                                  const unsigned char *pixels, 
                                  int width, int height,
                                  int pixel_size,
                                  unsigned long buffer_length,
                                  const char *blend_with_name,
                                  const char *color_option)
{
  // Check params

  if (!interp)
    {
    vtkGenericWarningMacro(<< "Empty interpreter");
    return 0;
    }

  if (!photo_name || !photo_name[0])
    {
    vtkGenericWarningMacro(<< "Empty photo name");
    return 0;
    }

  if (!pixels)
    {
    vtkGenericWarningMacro(<< "No pixel data");
    return 0;
    }

  if (width <= 0 || height <= 0)
    {
    vtkGenericWarningMacro(<< "Invalid size: " << width << "x" << height);
    return 0;
    }

  if (pixel_size != 3 && pixel_size != 4)
    {
    vtkGenericWarningMacro(<< "Unsupported pixel size: " << pixel_size);
    return 0;
    }

  // Find the photo (create it if not found)

  Tk_PhotoHandle photo = Tk_FindPhoto(interp, const_cast<char *>(photo_name));

  if (!photo)
    {
    ostrstream create_photo;
    create_photo << "image create photo " << photo_name << ends;
    int res = Tcl_GlobalEval(interp, create_photo.str());
    create_photo.rdbuf()->freeze(0);
    if (res != TCL_OK)
      {
      vtkGenericWarningMacro(
        << "Unable to create photo " << photo_name << ": " << interp->result);
      return 0;
      }

    photo = Tk_FindPhoto(interp, const_cast<char *>(photo_name));
    if (!photo)
      {
      vtkGenericWarningMacro(<< "Error looking up Tk photo:" << photo_name);
      return 0;
      }
    }

  Tk_PhotoSetSize(photo, width, height);

#if (TK_MAJOR_VERSION == 8) && (TK_MINOR_VERSION < 4)
  Tk_PhotoBlank(photo);
#endif

  unsigned long nb_of_raw_bytes = width * height * pixel_size;
  const unsigned char *data_ptr = pixels;

  // If the buffer_lenth has been provided, and if it's different than the
  // expected size of the raw image buffer, than it might have been compressed
  // using zlib and/or encoded in base64. In that case, decode and/or
  // uncompress the buffer.

  int base64 = 0;
  unsigned char *base64_buffer = 0;

  int zlib = 0;
  unsigned char *zlib_buffer = 0;

  if (buffer_length && buffer_length != nb_of_raw_bytes)
    {
    // Is it a base64 stream (i.e. not zlib for the moment) ?

    if (data_ptr[0] != 0x78 || data_ptr[1] != 0xDA)
      {
      base64_buffer = new unsigned char [buffer_length];
      buffer_length = vtkBase64Utility::Decode(data_ptr, 0, 
                                               base64_buffer, buffer_length);
      if (buffer_length == 0)
        {
        vtkGenericWarningMacro(<< "Error decoding base64 stream");
        delete [] base64_buffer;
        return 0;
        }
      base64 = 1;
      data_ptr = base64_buffer;
      }
    
    // Is it zlib ?

    if (buffer_length != nb_of_raw_bytes &&
        data_ptr[0] == 0x78 && data_ptr[1] == 0xDA)
      {
      unsigned long zlib_buffer_length = nb_of_raw_bytes;
      zlib_buffer = new unsigned char [zlib_buffer_length];
      if (uncompress(zlib_buffer, &zlib_buffer_length, 
                     data_ptr, buffer_length) != Z_OK ||
          zlib_buffer_length != nb_of_raw_bytes)
        {
        vtkGenericWarningMacro(<< "Error decoding zlib stream");
        delete [] zlib_buffer;
        if (base64)
          {
          delete [] base64_buffer;
          }
        return 0;
        }
      zlib = 1;
      data_ptr = zlib_buffer;
      }
    }

  // Set block struct

  Tk_PhotoImageBlock sblock;

  sblock.width     = width;
  sblock.height    = height;
  sblock.pixelSize = 3;
  sblock.pitch     = width * sblock.pixelSize;
  sblock.offset[0] = 0;
  sblock.offset[1] = 1;
  sblock.offset[2] = 2;
#if (TK_MAJOR_VERSION == 8) && (TK_MINOR_VERSION >= 3)
  sblock.offset[3] = 0;
#endif

  if (pixel_size == 3)
    {
    sblock.pixelPtr = const_cast<unsigned char *>(data_ptr);
    }
  else 
    {
    unsigned char *pp = sblock.pixelPtr = 
      new unsigned char[sblock.width * sblock.height * sblock.pixelSize];

    // At the moment let's not use the alpha layer inside the photo but 
    // blend with the current background color

    int r, g, b;
    if (blend_with_name)
      {
      vtkKWTkUtilities::GetOptionColor(interp, 
                                       blend_with_name, 
                                       (color_option ? color_option : "-bg"), 
                                       &r, &g, &b);
      }
    else
      {
      vtkKWTkUtilities::GetBackgroundColor(interp, ".", &r, &g, &b);
      }

    // Create photo pixels

    int xx, yy;
    for (yy=0; yy < height; yy++)
      {
      for (xx=0; xx < width; xx++)
        {
        float alpha = static_cast<float>(*(data_ptr + 3)) / 255.0;
        
        *(pp)     = static_cast<int>(r * (1 - alpha) + *(data_ptr)    * alpha);
        *(pp + 1) = static_cast<int>(g * (1 - alpha) + *(data_ptr+ 1) * alpha);
        *(pp + 2) = static_cast<int>(b * (1 - alpha) + *(data_ptr+ 2) * alpha);

        data_ptr += pixel_size;
        pp += 3;
        }
      }
    }

#if (TK_MAJOR_VERSION == 8) && (TK_MINOR_VERSION >= 4) && !defined(USE_COMPOSITELESS_PHOTO_PUT_BLOCK)
  Tk_PhotoPutBlock(photo, &sblock, 0, 0, width, height, TK_PHOTO_COMPOSITE_SET);
#else
  Tk_PhotoPutBlock(photo, &sblock, 0, 0, width, height);
#endif

  // Free mem

  if (pixel_size != 3)
    {
    delete [] sblock.pixelPtr;
    }

  if (base64)
    {
    delete [] base64_buffer;
    }

  if (zlib)
    {
    delete [] zlib_buffer;
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::UpdatePhoto(Tcl_Interp *interp,
                                  const char *photo_name,
                                  vtkImageData *image, 
                                  const char *blend_with_name,
                                  const char *color_option)
{
  vtkImageData *input = image;

  if (!input)
    {
    vtkGenericWarningMacro(<< "No image data specified");
    return 0;
    }
  input->Update();

#define FLIP 1
#if FLIP
  vtkImageFlip *flip = vtkImageFlip::New();
  flip->SetInput(input);
  flip->SetFilteredAxis(1);
  flip->Update();
  input = flip->GetOutput();
#endif

  int *ext = input->GetWholeExtent();
  if ((ext[5] - ext[4]) > 0)
    {
    vtkGenericWarningMacro(<< "Can only handle 2D input data");
#if FLIP
    flip->Delete();
#endif
    return 0;
    }

  int width = ext[1] - ext[0] + 1;
  int height = ext[3] - ext[2] + 1;
  int pixel_size = input->GetNumberOfScalarComponents();

  int res = vtkKWTkUtilities::UpdatePhoto(
    interp,
    photo_name,
    static_cast<unsigned char*>(input->GetScalarPointer()),
    width, height,
    pixel_size,
    width * height * pixel_size,
    blend_with_name,
    color_option);

#if FLIP
  flip->Delete();
#endif
  return res;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetPhotoHeight(Tcl_Interp *interp,
                                     const char *photo_name)
{
  // Find the photo

  Tk_PhotoHandle photo = Tk_FindPhoto(interp,
                                      const_cast<char *>(photo_name));
  if (!photo)
    {
    vtkGenericWarningMacro(<< "Error looking up Tk photo:" << photo_name);
    return 0;
    }  

  // Return height

  int width, height;
  Tk_PhotoGetSize(photo, &width, &height);
  return height;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetPhotoWidth(Tcl_Interp *interp,
                                    const char *photo_name)
{
  // Find the photo

  Tk_PhotoHandle photo = Tk_FindPhoto(interp,
                                      const_cast<char *>(photo_name));
  if (!photo)
    {
    vtkGenericWarningMacro(<< "Error looking up Tk photo:" << photo_name);
    return 0;
    }  

  // Return width

  int width, height;
  Tk_PhotoGetSize(photo, &width, &height);
  return width;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::ChangeFontToBold(Tcl_Interp *interp,
                                       const char *widget)
{
  int res;

  // First try to modify the old -foundry-family-weigth-*-*-... form

  ostrstream regsub;
  regsub << "regsub -- {(-[^-]*-[^-]*-)([^-]*)(-.*)} [" << widget 
          << " cget -font] {\\1bold\\3} __temp__" << ends;
  res = Tcl_GlobalEval(interp, regsub.str());
  regsub.rdbuf()->freeze(0);
  if (res != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to regsub!");
    return 0;
    }
  if (atoi(Tcl_GetStringResult(interp)) == 1)
    {
    ostrstream replace;
    replace << widget << " config -font $__temp__" << ends;
    res = Tcl_GlobalEval(interp, replace.str());
    replace.rdbuf()->freeze(0);
    if (res != TCL_OK)
      {
      vtkGenericWarningMacro(<< "Unable to replace result of regsub!");
      return 0;
      }
    return 1;
    }

  // Otherwise replace the -weight parameter

  ostrstream regsub2;
  regsub2 << "regsub -- {(.* -weight )(\\w*\\M)(.*)} [font actual [" << widget 
          << " cget -font]] {\\1bold\\3} __temp__" << ends;
  res = Tcl_GlobalEval(interp, regsub2.str());
  regsub2.rdbuf()->freeze(0);
  if (res != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to regsub (2)!");
    return 0;
    }
  if (atoi(Tcl_GetStringResult(interp)) == 1)
    {
    ostrstream replace2;
    replace2 << widget << " config -font $__temp__" << ends;
    res = Tcl_GlobalEval(interp, replace2.str());
    replace2.rdbuf()->freeze(0);
    if (res != TCL_OK)
      {
      vtkGenericWarningMacro(<< "Unable to replace result of regsub (2)!");
      return 0;
      }
    return 1;
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetGridSize(Tcl_Interp *interp,
                                  const char *widget,
                                  int *nb_of_cols,
                                  int *nb_of_rows)
{
  ostrstream size;
  size << "grid size " << widget << ends;
  int res = Tcl_GlobalEval(interp, size.str());
  size.rdbuf()->freeze(0);
  if (res != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to query grid size!");
    return 0;
    }
  sscanf(interp->result, "%d %d", nb_of_cols, nb_of_rows);

  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetPackSlavePadding(Tcl_Interp *interp,
                                          const char *widget,
                                          int *ipadx,
                                          int *ipady,
                                          int *padx,
                                          int *pady)
{
  ostrstream packinfo;
  packinfo << "pack info " << widget << ends;
  int res = Tcl_GlobalEval(interp, packinfo.str());
  packinfo.rdbuf()->freeze(0);
  if (res != TCL_OK || !interp->result || !interp->result[0])
    {
    vtkGenericWarningMacro(<< "Unable to get pack info!");
    return 0;
    }
  
  // Parse (ex: -ipadx 0 -ipady 0 -padx 0 -pady 0)

  char *ptr = strstr(interp->result, "-ipadx ");
  if (ptr)
    {
    sscanf(ptr + 7, "%d", ipadx);
    }
  ptr = strstr(interp->result, "-ipady ");
  if (ptr)
    {
    sscanf(ptr + 7, "%d", ipady);
    }
  ptr = strstr(interp->result, "-padx ");
  if (ptr)
    {
    sscanf(ptr + 6, "%d", padx);
    }
  ptr = strstr(interp->result, "-pady ");
  if (ptr)
    {
    sscanf(ptr + 6, "%d", pady);
    }
  
  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetPackSlavesBbox(Tcl_Interp *interp,
                                        const char *widget,
                                        int *width,
                                        int *height)
{
  ostrstream slaves;
  slaves << "pack slaves " << widget << ends;
  int res = Tcl_GlobalEval(interp, slaves.str());
  slaves.rdbuf()->freeze(0);
  if (res != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to get pack slaves!");
    return 0;
    }
  
  // No slaves
  
  if (!interp->result || !interp->result[0])
    {
    return 1;
    }
  
  // Browse each slave for reqwidth, reqheight

  int buffer_length = strlen(interp->result);
  char *buffer = new char [buffer_length + 1];
  strcpy(buffer, interp->result);
  char *buffer_end = buffer + buffer_length;
  char *ptr = buffer, *word_end;

  while (ptr < buffer_end)
    {
    // Get the slave name

    word_end = strchr(ptr + 1, ' ');
    if (word_end == NULL)
      {
      word_end = buffer_end;
      }
    else
      {
      *word_end = 0;
      }

    // Get width / height

    ostrstream geometry;
    geometry << "concat [winfo reqwidth " << ptr << "] [winfo reqheight " 
             << ptr << "]"<< ends;
    res = Tcl_GlobalEval(interp, geometry.str());
    geometry.rdbuf()->freeze(0);
    if (res != TCL_OK)
      {
      vtkGenericWarningMacro(<< "Unable to query slave geometry!");
      }
    else
      {
      int w, h;
      sscanf(interp->result, "%d %d", &w, &h);

      // If w == h == 1 then again it might not have been packed, so call
      // recursively

      if (w == 1 && h == 1)
        {
        vtkKWTkUtilities::GetPackSlavesBbox(interp, ptr, &w, &h);
        }

      // Don't forget the padding

      int ipadx = 0, ipady = 0, padx = 0, pady = 0;
      vtkKWTkUtilities::GetPackSlavePadding(interp, ptr, 
                                            &ipadx, &ipady, &padx, &pady);

      w += 2 * (padx + ipadx);
      h += 2 * (pady + ipady);

      if (w > *width)
        {
        *width = w;
        }
      if (h > *height)
        {
        *height = h;
        }
      }
    
    ptr = word_end + 1;
    }

  delete [] buffer;

  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::GetGridColumnWidths(Tcl_Interp *interp,
                                          const char *widget,
                                          int *nb_of_cols,
                                          int **col_widths,
                                          int allocate)
{
  // First get grid size

  int nb_of_rows;
  if (!vtkKWTkUtilities::GetGridSize(interp, widget, nb_of_cols, &nb_of_rows))
    {
    vtkGenericWarningMacro(<< "Unable to query grid size!");
    return 0;
    }

  // Iterate over the columns and get the largest widget
  // (I'm expecting only one widget per cell here)

  if (allocate)
    {
    *col_widths = new int[*nb_of_cols];
    }

  int col, row;
  for (col = 0; col < *nb_of_cols; col++)
    {
    (*col_widths)[col] = 0;
    for (row = 0; row < nb_of_rows; row++)
      {
      // Get the slave

      ostrstream slave;
      slave << "grid slaves " << widget << " -column " << col 
            << " -row " << row << ends;
      int res = Tcl_GlobalEval(interp, slave.str());
      slave.rdbuf()->freeze(0);
      if (res != TCL_OK)
        {
        vtkGenericWarningMacro(<< "Unable to get grid slave!");
        continue;
        }

      // No slave, let's process the next row

      if (!interp->result || !interp->result[0])
        {
        continue;
        }

      // Get the slave reqwidth

      ostrstream reqwidth;
      reqwidth << "winfo reqwidth " << interp->result << ends;
      res = Tcl_GlobalEval(interp, reqwidth.str());
      reqwidth.rdbuf()->freeze(0);
      if (res != TCL_OK)
        {
        vtkGenericWarningMacro(<< "Unable to query slave width!");
        continue;
        }
      int width = 0;
      sscanf(interp->result, "%d", &width);

      if (width > (*col_widths)[col])
        {
        (*col_widths)[col] = width;
        }
      }
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkKWTkUtilities::SynchroniseGridsColumnMinimumSize(
  Tcl_Interp *interp,
  int nb_of_widgets,
  const char **widgets,
  const float *factors,
  const int *weights)
{
  // Allocate mem for nb of colums and widths

  int *nb_of_cols = new int [nb_of_widgets];
  int **col_widths = new int* [nb_of_widgets];
  int widget;

  // Collect column widths

  int min_nb_of_cols = 10000;
  for (widget = 0; widget < nb_of_widgets; widget++)
    {
    if (vtkKWTkUtilities::GetGridColumnWidths(
      interp, widgets[widget], &nb_of_cols[widget], &col_widths[widget], 1))
      {
      if (nb_of_cols[widget] < min_nb_of_cols)
        {
        min_nb_of_cols = nb_of_cols[widget];
        }
      }
    }

  // Synchronize columns (for each column, configure -minsize to the largest
  // column width for all grids)

  ostrstream minsize;
  for (int col = 0; col < min_nb_of_cols; col++)
    {
    int col_width_max = 0;
    for (widget = 0; widget < nb_of_widgets; widget++)
      {
      if (col_widths[widget][col] > col_width_max)
        {
        col_width_max = col_widths[widget][col];
        }
      }
    if (factors)
      {
      col_width_max = (int)((float)col_width_max * factors[col]);
      }
    for (widget = 0; widget < nb_of_widgets; widget++)
      {
      minsize << "grid columnconfigure " << widgets[widget] << " " << col 
              << " -minsize " << col_width_max;
      if (weights)
        {
        minsize << " -weight " << weights[col];
        }
      minsize << endl;
      }
    }
  minsize << ends;
  // cout << minsize.str() << endl;

  int ok = 1;
  if (Tcl_GlobalEval(interp, minsize.str()) != TCL_OK)
    {
    vtkGenericWarningMacro(<< "Unable to synchronize grid columns!");
    ok = 0;
    }
  minsize.rdbuf()->freeze(0);

  // Free mem

  delete [] nb_of_cols;
  for (widget = 0; widget < nb_of_widgets; widget++)
    {
    delete [] col_widths[widget];
    }
  delete [] col_widths;

  return ok;
}

//----------------------------------------------------------------------------
void vtkKWTkUtilities::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
