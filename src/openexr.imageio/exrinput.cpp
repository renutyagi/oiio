/////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2008 Larry Gritz
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// 
// (this is the MIT license)
/////////////////////////////////////////////////////////////////////////////


#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>

#include <boost/algorithm/string.hpp>
using boost::algorithm::iequals;

#include <ImfTestFile.h>
#include <ImfInputFile.h>
#include <ImfTiledInputFile.h>
#include <ImfChannelList.h>
#include <ImfEnvmap.h>
#include <ImfIntAttribute.h>
#include <ImfFloatAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfEnvmapAttribute.h>
#include <ImfCompressionAttribute.h>
//using namespace Imf;

#include "dassert.h"
#include "paramtype.h"
#include "imageio.h"
#include "thread.h"
#include "strutil.h"

using namespace OpenImageIO;



class OpenEXRInput : public ImageInput {
public:
    OpenEXRInput () { init(); }
    virtual ~OpenEXRInput () { close(); }
    virtual const char * format_name (void) const { return "OpenEXR"; }
    virtual bool open (const char *name, ImageIOFormatSpec &newspec);
    virtual bool close ();
    virtual int current_subimage (void) const { return m_subimage; }
    virtual bool seek_subimage (int index, ImageIOFormatSpec &newspec);
    virtual bool read_native_scanline (int y, int z, void *data);
    virtual bool read_native_tile (int x, int y, int z, void *data);

private:
    const Imf::Header *m_header;          ///< Ptr to image header
    Imf::InputFile *m_input_scanline;     ///< Input for scanline files
    Imf::TiledInputFile *m_input_tiled;   ///< Input for tiled files
    std::string m_filename;               ///< Stash the filename
    int m_levelmode;                      ///< The level mode of the file
    int m_roundingmode;                   ///< Rounding mode of the file
    int m_subimage;                       ///< What subimage are we looking at?
    int m_nsubimages;                     ///< How many subimages are there?
    int m_topwidth;                       ///< Width of top mip level
    int m_topheight;                      ///< Height of top mip level
    bool m_cubeface;                      ///< It's a cubeface environment map
    std::vector<std::string> m_channelnames;  ///< Order of channels in file
    std::vector<int> m_userchannels;      ///< Map file chans to user chans
    std::vector<unsigned char> m_scratch; ///< Scratch space for us to use

    void init () {
        m_header = NULL;
        m_input_scanline = NULL;
        m_input_tiled = NULL;
        m_subimage = -1;
    }

    // Helper for open(): set up m_spec.nchannels, m_spec.channelnames,
    // m_spec.alpha_channel, m_spec.z_channel, m_channelnames,
    // m_userchannels.
    void query_channels (void);
};



// Obligatory material to make this a recognizeable imageio plugin:
extern "C" {

DLLEXPORT OpenEXRInput *
openexr_input_imageio_create ()
{
    return new OpenEXRInput;
}

// DLLEXPORT int imageio_version = IMAGEIO_VERSION; // it's in tiffoutput.cpp

DLLEXPORT const char * openexr_input_extensions[] = {
    "exr", NULL
};

};



class StringMap {
public:
    StringMap (void) { init(); }

    const std::string & operator[] (const std::string &s) const {
        std::map<std::string, std::string>::const_iterator i;
        i = m_map.find (s);
        return i == m_map.end() ? s : i->second;
    }
private:
    std::map<std::string, std::string> m_map;

    void init (void) {
        // Ones whose name we change to our convention
        m_map["cameraTransform"] = "worldtocamera";
        m_map["capDate"] = "datetime";
        m_map["comments"] = "description";
        m_map["owner"] = "copyright";
        m_map["pixelAspectRatio"] = "pixelaspectratio";
        // Ones we don't rename -- OpenEXR convention matches ours
        m_map["wrapmodes"] = "wrapmodes";
        // Ones to skip because we handle specially
        m_map["channels"] = "";
        m_map["compression"] = "";
        m_map["dataWindow"] = "";
        m_map["envmap"] = "";
        m_map["tiledesc"] = "";
        // Ones to skip because we consider them irrelevant
        m_map["lineOrder"] = "";

//        m_map[""] = "";
        // FIXME: Things to consider in the future:
        // displayWindow
        // preview
        // screenWindowCenter
        // chromaticities whiteLuminance adoptedNeutral
        // renderingTransform, lookModTransform
        // xDensity
        // utcOffset
        // longitude latitude altitude
        // focus expTime aperture isoSpeed
        // keyCode timeCode framesPerSecond
    }
};

static StringMap exr_tag_to_ooio_std;



bool
OpenEXRInput::open (const char *name, ImageIOFormatSpec &newspec)
{
    // Quick check to reject non-exr files
    bool tiled;
    if (! Imf::isOpenExrFile (name, tiled))
        return false;

    m_spec = ImageIOFormatSpec(); // Clear everything with default constructor
    try {
        if (tiled) {
            m_input_tiled = new Imf::TiledInputFile (name);
            m_header = &(m_input_tiled->header());
        } else {
            m_input_scanline = new Imf::InputFile (name);
            m_header = &(m_input_scanline->header());
        }
    }
    catch (const std::exception &e) {
        error ("OpenEXR exception: %s", e.what());
        return false;
    }
    if (! m_input_scanline && ! m_input_tiled) {
        error ("Unknown error opening EXR file");
        return false;
    }

    Imath::Box2i datawindow = m_header->dataWindow();
    m_spec.x = datawindow.min.x;
    m_spec.y = datawindow.min.y;
    m_spec.z = 0;
    m_spec.width = datawindow.max.x - datawindow.min.x + 1;
    m_spec.height = datawindow.max.y - datawindow.min.y + 1;
    m_spec.depth = 1;
    m_topwidth = m_spec.width;      // Save top-level mipmap dimensions
    m_topheight = m_spec.height;
    m_spec.full_width = m_spec.width;
    m_spec.full_height = m_spec.height;
    m_spec.full_depth = m_spec.depth;
    if (tiled) {
        m_spec.tile_width = m_input_tiled->tileXSize();
        m_spec.tile_height = m_input_tiled->tileYSize();
    } else {
        m_spec.tile_width = 0;
        m_spec.tile_height = 0;
    }
    m_spec.tile_depth = 0;
    m_spec.format = PT_HALF;  // FIXME: do the right thing for non-half
    query_channels ();

    if (tiled) {
        // FIXME: levelmode
        m_levelmode = m_input_tiled->levelMode();
        m_roundingmode = m_input_tiled->levelRoundingMode();
        if (m_levelmode == Imf::MIPMAP_LEVELS)
            m_nsubimages = m_input_tiled->numLevels();
        else if (m_levelmode == Imf::RIPMAP_LEVELS)
            m_nsubimages = std::max (m_input_tiled->numXLevels(),
                                     m_input_tiled->numYLevels());
        else
            m_nsubimages = 1;
    } else {
        m_levelmode = Imf::ONE_LEVEL;
        m_nsubimages = 1;
    }

    const Imf::EnvmapAttribute *envmap;
    envmap = m_header->findTypedAttribute<Imf::EnvmapAttribute>("envmap");
    if (envmap) {
        m_cubeface = (envmap->value() == Imf::ENVMAP_CUBE);
        m_spec.add_parameter ("textureformat", m_cubeface ? "CubeFace Environment" : "LatLong Environment");
        // FIXME - detect CubeFace Shadow
        m_spec.add_parameter ("up", "y");  // OpenEXR convention
    } else {
        m_cubeface = false;
        if (tiled)
            m_spec.add_parameter ("textureformat", "Plain Texture");
        // FIXME - detect Shadow
    }

    const Imf::CompressionAttribute *compressattr;
    compressattr = m_header->findTypedAttribute<Imf::CompressionAttribute>("compression");
    if (compressattr) {
        const char *comp = NULL;
        switch (compressattr->value()) {
        case Imf::NO_COMPRESSION    : comp = "none"; break;
        case Imf::RLE_COMPRESSION   : comp = "rle"; break;
        case Imf::ZIPS_COMPRESSION  : comp = "zip"; break;
        case Imf::ZIP_COMPRESSION   : comp = "zip"; break;
        case Imf::PIZ_COMPRESSION   : comp = "piz"; break;
        case Imf::PXR24_COMPRESSION : comp = "pxr24"; break;
        case Imf::B44_COMPRESSION   : comp = "b44"; break;
        case Imf::B44A_COMPRESSION  : comp = "b44a"; break;
        }
        if (comp)
            m_spec.add_parameter ("compression", comp);
    }

    for (Imf::Header::ConstIterator hit = m_header->begin();
             hit != m_header->end();  ++hit) {
        const Imf::IntAttribute *iattr;
        const Imf::FloatAttribute *fattr;
        const Imf::StringAttribute *sattr;
        const Imf::M44fAttribute *mattr;
        const char *name = hit.name();
        std::string oname = exr_tag_to_ooio_std[name];
        if (oname.empty())   // Empty string means skip this attrib
            continue;
        if (oname == name)
            oname = std::string("openexr_") + oname;
        const Imf::Attribute &attrib = hit.attribute();
        std::string type = attrib.typeName();
        if (type == "string" && 
            (sattr = m_header->findTypedAttribute<Imf::StringAttribute> (name)))
            m_spec.add_parameter (oname, sattr->value().c_str());
        else if (type == "int" && 
            (iattr = m_header->findTypedAttribute<Imf::IntAttribute> (name)))
            m_spec.add_parameter (oname, iattr->value());
        else if (type == "float" && 
            (fattr = m_header->findTypedAttribute<Imf::FloatAttribute> (name)))
            m_spec.add_parameter (oname, fattr->value());
        else if (type == "M44f" && 
            (mattr = m_header->findTypedAttribute<Imf::M44fAttribute> (name)))
            m_spec.add_parameter (oname, PT_MATRIX, 1, &(mattr->value()));
        else {
            std::cerr << "  unknown attribute " << type << ' ' << name << "\n";
        }
    }

    m_subimage = 0;
    newspec = m_spec;
    return true;
}



void
OpenEXRInput::query_channels (void)
{
    m_spec.nchannels = 0;
    const Imf::ChannelList &channels (m_header->channels());
    Imf::ChannelList::ConstIterator ci;
    int c;
    int red = -1, green = -1, blue = -1, alpha = -1, zee = -1;
    for (c = 0, ci = channels.begin();  ci != channels.end();  ++c, ++ci) {
        // std::cerr << "Channel " << ci.name() << '\n';
        std::string name = ci.name();
        m_channelnames.push_back (name);
        if (iequals(name, "R") || iequals(name, "Red"))
            red = c;
        if (iequals(name, "G") || iequals(name, "Green"))
            green = c;
        if (iequals(name, "B") || iequals(name, "Blue"))
            blue = c;
        if (iequals(name, "A") || iequals(name, "Alpha"))
            alpha = c;
        else if (iequals(name, "Z"))
            zee = c;
        ++m_spec.nchannels;
    }
    m_userchannels.resize (m_spec.nchannels);
    int nc = 0;
    if (red >= 0) {
        m_spec.channelnames.push_back ("R");
        m_userchannels[red] = nc++;
    }
    if (green >= 0) {
        m_spec.channelnames.push_back ("G");
        m_userchannels[green] = nc++;
    }
    if (blue >= 0) {
        m_spec.channelnames.push_back ("B");
        m_userchannels[blue] = nc++;
    }
    if (alpha >= 0) {
        m_spec.channelnames.push_back ("A");
        m_userchannels[alpha] = nc++;
    }
    if (zee >= 0) {
        m_spec.channelnames.push_back ("Z");
        m_userchannels[zee] = nc++;
    }
    for (c = 0, ci = channels.begin();  ci != channels.end();  ++c, ++ci) {
        if (red == c || green == c || blue == c || alpha == c || zee == c)
            continue;   // Already accounted for this channel
        m_userchannels[c] = nc;
        m_spec.channelnames.push_back (ci.name());
        ++nc;
    }
    ASSERT (m_spec.channelnames.size() == m_spec.nchannels);
    // FIXME: should we also figure out the layers?
}



bool
OpenEXRInput::seek_subimage (int index, ImageIOFormatSpec &newspec)
{
    if (index < 0 || index >= m_nsubimages)   // out of range
        return false;

    m_subimage = index;

    if (index == 0 && m_levelmode == Imf::ONE_LEVEL) {
        newspec = m_spec;
        return true;
    }

    // Compute the resolution of the requested subimage.
    int w = m_topwidth, h = m_topheight;
    if (m_levelmode == Imf::MIPMAP_LEVELS) {
        while (index--) {
            if (w > 1) {
                if ((w & 1) && m_roundingmode == Imf::ROUND_UP)
                    w = w/2 + 1;
                else w /= 2;
            }
            if (h > 1) {
                if ((h & 1) && m_roundingmode == Imf::ROUND_UP)
                    h = h/2 + 1;
                else h /= 2;
            }
        }
    } else if (m_levelmode == Imf::RIPMAP_LEVELS) {
        // FIXME
    } else {
        ASSERT(0);
    }

    m_spec.width = w;
    m_spec.height = h;
    m_spec.full_width = w;
    m_spec.full_height = m_cubeface ? w : h;
    newspec = m_spec;

    return true;
}



bool
OpenEXRInput::close ()
{
    delete m_input_scanline;
    delete m_input_tiled;
    m_subimage = -1;
    init ();  // Reset to initial state
    return true;
}



bool
OpenEXRInput::read_native_scanline (int y, int z, void *data)
{
    ASSERT (m_input_scanline != NULL);

    // Compute where OpenEXR needs to think the full buffers starts.
    // OpenImageIO requires that 'data' points to where the client wants
    // to put the pixels being read, but OpenEXR's frameBuffer.insert()
    // wants where the address of the "virtual framebuffer" for the
    // whole image.
    char *buf = (char *)data
              - m_spec.x * m_spec.pixel_bytes() 
              - (y + m_spec.y) * m_spec.scanline_bytes();

    try {
        Imf::FrameBuffer frameBuffer;
        for (int c = 0;  c < m_spec.nchannels;  ++c) {
            frameBuffer.insert (m_spec.channelnames[c].c_str(),
                                Imf::Slice (Imf::HALF,  // FIXME
                                            buf + c * m_spec.channel_bytes(),
                                            m_spec.pixel_bytes(),
                                            m_spec.scanline_bytes()));
            // FIXME - what if all channels aren't the same data type?
        }
        m_input_scanline->setFrameBuffer (frameBuffer);
        y -= m_spec.y;
        m_input_scanline->readPixels (y, y);
    }
    catch (const std::exception &e) {
        error ("Filed OpenEXR read: %s", e.what());
        return false;
    }
    return true;
}



bool
OpenEXRInput::read_native_tile (int x, int y, int z, void *data)
{
    ASSERT (m_input_tiled != NULL);

    // Compute where OpenEXR needs to think the full buffers starts.
    // OpenImageIO requires that 'data' points to where the client wants
    // to put the pixels being read, but OpenEXR's frameBuffer.insert()
    // wants where the address of the "virtual framebuffer" for the
    // whole image.
    char *buf = (char *)data
              - m_spec.x * m_spec.pixel_bytes() 
              - (y + m_spec.y) * m_spec.scanline_bytes();

    try {
        Imf::FrameBuffer frameBuffer;
        for (int c = 0;  c < m_spec.nchannels;  ++c) {
            frameBuffer.insert (m_spec.channelnames[c].c_str(),
                                Imf::Slice (Imf::HALF,  // FIXME
                                            buf + c * m_spec.channel_bytes(),
                                            m_spec.pixel_bytes(),
                                            m_spec.scanline_bytes()));
            // FIXME - what if all channels aren't the same data type?
        }
        m_input_tiled->setFrameBuffer (frameBuffer);
        x -= m_spec.x;
        y -= m_spec.y;
        m_input_tiled->readTile (x/m_spec.tile_width, y/m_spec.tile_height,
                                 m_subimage, m_subimage);
    }
    catch (const std::exception &e) {
        error ("Filed OpenEXR read: %s", e.what());
        return false;
    }
    return true;
}