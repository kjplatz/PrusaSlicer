#include "../libslic3r.h"
#include "../Model.hpp"
#include "../Utils.hpp"
#include "../GCode.hpp"
#include "../Geometry.hpp"
#if ENABLE_THUMBNAIL_GENERATOR
#include "../GCode/ThumbnailData.hpp"
#endif // ENABLE_THUMBNAIL_GENERATOR

#include "../I18N.hpp"

#include "3mf.hpp"

#include <limits>
#include <stdexcept>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
namespace pt = boost::property_tree;

#include <expat.h>
#include <Eigen/Dense>
#include "miniz_extension.hpp"

// VERSION NUMBERS
// 0 : .3mf, files saved by older slic3r or other applications. No version definition in them.
// 1 : Introduction of 3mf versioning. No other change in data saved into 3mf files.
// 2 : Meshes saved in their local system; Volumes' matrices and source data added to Metadata/Slic3r_PE_model.config file.
const unsigned int VERSION_3MF = 2;
const char* SLIC3RPE_3MF_VERSION = "slic3rpe:Version3mf"; // definition of the metadata name saved into .model file

const std::string MODEL_FOLDER = "3D/";
const std::string MODEL_EXTENSION = ".model";
const std::string MODEL_FILE = "3D/3dmodel.model"; // << this is the only format of the string which works with CURA
const std::string CONTENT_TYPES_FILE = "[Content_Types].xml";
const std::string RELATIONSHIPS_FILE = "_rels/.rels";
#if ENABLE_THUMBNAIL_GENERATOR
const std::string THUMBNAIL_FILE = "Metadata/thumbnail.png";
#endif // ENABLE_THUMBNAIL_GENERATOR
const std::string PRINT_CONFIG_FILE = "Metadata/Slic3r_PE.config";
const std::string MODEL_CONFIG_FILE = "Metadata/Slic3r_PE_model.config";
const std::string LAYER_HEIGHTS_PROFILE_FILE = "Metadata/Slic3r_PE_layer_heights_profile.txt";
const std::string LAYER_CONFIG_RANGES_FILE = "Metadata/Prusa_Slicer_layer_config_ranges.xml";
const std::string SLA_SUPPORT_POINTS_FILE = "Metadata/Slic3r_PE_sla_support_points.txt";

const char* MODEL_TAG = "model";
const char* RESOURCES_TAG = "resources";
const char* OBJECT_TAG = "object";
const char* MESH_TAG = "mesh";
const char* VERTICES_TAG = "vertices";
const char* VERTEX_TAG = "vertex";
const char* TRIANGLES_TAG = "triangles";
const char* TRIANGLE_TAG = "triangle";
const char* COMPONENTS_TAG = "components";
const char* COMPONENT_TAG = "component";
const char* BUILD_TAG = "build";
const char* ITEM_TAG = "item";
const char* METADATA_TAG = "metadata";

const char* CONFIG_TAG = "config";
const char* VOLUME_TAG = "volume";

const char* UNIT_ATTR = "unit";
const char* NAME_ATTR = "name";
const char* TYPE_ATTR = "type";
const char* ID_ATTR = "id";
const char* X_ATTR = "x";
const char* Y_ATTR = "y";
const char* Z_ATTR = "z";
const char* V1_ATTR = "v1";
const char* V2_ATTR = "v2";
const char* V3_ATTR = "v3";
const char* OBJECTID_ATTR = "objectid";
const char* TRANSFORM_ATTR = "transform";
const char* PRINTABLE_ATTR = "printable";

const char* KEY_ATTR = "key";
const char* VALUE_ATTR = "value";
const char* FIRST_TRIANGLE_ID_ATTR = "firstid";
const char* LAST_TRIANGLE_ID_ATTR = "lastid";

const char* OBJECT_TYPE = "object";
const char* VOLUME_TYPE = "volume";

const char* NAME_KEY = "name";
const char* MODIFIER_KEY = "modifier";
const char* VOLUME_TYPE_KEY = "volume_type";
const char* MATRIX_KEY = "matrix";
const char* SOURCE_FILE_KEY = "source_file";
const char* SOURCE_OBJECT_ID_KEY = "source_object_id";
const char* SOURCE_VOLUME_ID_KEY = "source_volume_id";
const char* SOURCE_OFFSET_X_KEY = "source_offset_x";
const char* SOURCE_OFFSET_Y_KEY = "source_offset_y";
const char* SOURCE_OFFSET_Z_KEY = "source_offset_z";

const unsigned int VALID_OBJECT_TYPES_COUNT = 1;
const char* VALID_OBJECT_TYPES[] =
{
    "model"
};

const unsigned int INVALID_OBJECT_TYPES_COUNT = 4;
const char* INVALID_OBJECT_TYPES[] =
{
    "solidsupport",
    "support",
    "surface",
    "other"
};

class version_error : public std::runtime_error
{
public:
    version_error(const std::string& what_arg) : std::runtime_error(what_arg) {}
    version_error(const char* what_arg) : std::runtime_error(what_arg) {}
};

const char* get_attribute_value_charptr(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    if ((attributes == nullptr) || (attributes_size == 0) || (attributes_size % 2 != 0) || (attribute_key == nullptr))
        return nullptr;

    for (unsigned int a = 0; a < attributes_size; a += 2)
    {
        if (::strcmp(attributes[a], attribute_key) == 0)
            return attributes[a + 1];
    }

    return nullptr;
}

std::string get_attribute_value_string(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? text : "";
}

float get_attribute_value_float(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? (float)::atof(text) : 0.0f;
}

int get_attribute_value_int(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? ::atoi(text) : 0;
}

bool get_attribute_value_bool(const char** attributes, unsigned int attributes_size, const char* attribute_key)
{
    const char* text = get_attribute_value_charptr(attributes, attributes_size, attribute_key);
    return (text != nullptr) ? (bool)::atoi(text) : true;
}

Slic3r::Transform3d get_transform_from_3mf_specs_string(const std::string& mat_str)
{
    // check: https://3mf.io/3d-manufacturing-format/ or https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md
    // to see how matrices are stored inside 3mf according to specifications
    Slic3r::Transform3d ret = Slic3r::Transform3d::Identity();

    if (mat_str.empty())
        // empty string means default identity matrix
        return ret;

    std::vector<std::string> mat_elements_str;
    boost::split(mat_elements_str, mat_str, boost::is_any_of(" "), boost::token_compress_on);

    unsigned int size = (unsigned int)mat_elements_str.size();
    if (size != 12)
        // invalid data, return identity matrix
        return ret;

    unsigned int i = 0;
    // matrices are stored into 3mf files as 4x3
    // we need to transpose them
    for (unsigned int c = 0; c < 4; ++c)
    {
        for (unsigned int r = 0; r < 3; ++r)
        {
            ret(r, c) = ::atof(mat_elements_str[i++].c_str());
        }
    }
    return ret;
}

float get_unit_factor(const std::string& unit)
{
    const char* text = unit.c_str();

    if (::strcmp(text, "micron") == 0)
        return 0.001f;
    else if (::strcmp(text, "centimeter") == 0)
        return 10.0f;
    else if (::strcmp(text, "inch") == 0)
        return 25.4f;
    else if (::strcmp(text, "foot") == 0)
        return 304.8f;
    else if (::strcmp(text, "meter") == 0)
        return 1000.0f;
    else
        // default "millimeters" (see specification)
        return 1.0f;
}

bool is_valid_object_type(const std::string& type)
{
    // if the type is empty defaults to "model" (see specification)
    if (type.empty())
        return true;

    for (unsigned int i = 0; i < VALID_OBJECT_TYPES_COUNT; ++i)
    {
        if (::strcmp(type.c_str(), VALID_OBJECT_TYPES[i]) == 0)
            return true;
    }

    return false;
}

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

    // Base class with error messages management
    class _3MF_Base
    {
        std::vector<std::string> m_errors;

    protected:
        void add_error(const std::string& error) { m_errors.push_back(error); }
        void clear_errors() { m_errors.clear(); }

    public:
        void log_errors()
        {
            for (const std::string& error : m_errors)
            {
                printf("%s\n", error.c_str());
            }
        }
    };

    class _3MF_Importer : public _3MF_Base
    {
        struct Component
        {
            int object_id;
            Transform3d transform;

            explicit Component(int object_id)
                : object_id(object_id)
                , transform(Transform3d::Identity())
            {
            }

            Component(int object_id, const Transform3d& transform)
                : object_id(object_id)
                , transform(transform)
            {
            }
        };

        typedef std::vector<Component> ComponentsList;

        struct Geometry
        {
            std::vector<float> vertices;
            std::vector<unsigned int> triangles;

            bool empty()
            {
                return vertices.empty() || triangles.empty();
            }

            void reset()
            {
                vertices.clear();
                triangles.clear();
            }
        };

        struct CurrentObject
        {
            // ID of the object inside the 3MF file, 1 based.
            int id;
            // Index of the ModelObject in its respective Model, zero based.
            int model_object_idx;
            Geometry geometry;
            ModelObject* object;
            ComponentsList components;

            CurrentObject()
            {
                reset();
            }

            void reset()
            {
                id = -1;
                model_object_idx = -1;
                geometry.reset();
                object = nullptr;
                components.clear();
            }
        };

        struct CurrentConfig
        {
            int object_id;
            int volume_id;
        };

        struct Instance
        {
            ModelInstance* instance;
            Transform3d transform;

            Instance(ModelInstance* instance, const Transform3d& transform)
                : instance(instance)
                , transform(transform)
            {
            }
        };

        struct Metadata
        {
            std::string key;
            std::string value;

            Metadata(const std::string& key, const std::string& value)
                : key(key)
                , value(value)
            {
            }
        };

        typedef std::vector<Metadata> MetadataList;

        struct ObjectMetadata
        {
            struct VolumeMetadata
            {
                unsigned int first_triangle_id;
                unsigned int last_triangle_id;
                MetadataList metadata;

                VolumeMetadata(unsigned int first_triangle_id, unsigned int last_triangle_id)
                    : first_triangle_id(first_triangle_id)
                    , last_triangle_id(last_triangle_id)
                {
                }
            };

            typedef std::vector<VolumeMetadata> VolumeMetadataList;

            MetadataList metadata;
            VolumeMetadataList volumes;
        };

        // Map from a 1 based 3MF object ID to a 0 based ModelObject index inside m_model->objects.
        typedef std::map<int, int> IdToModelObjectMap;
        typedef std::map<int, ComponentsList> IdToAliasesMap;
        typedef std::vector<Instance> InstancesList;
        typedef std::map<int, ObjectMetadata> IdToMetadataMap;
        typedef std::map<int, Geometry> IdToGeometryMap;
        typedef std::map<int, std::vector<coordf_t>> IdToLayerHeightsProfileMap;
        typedef std::map<int, t_layer_config_ranges> IdToLayerConfigRangesMap;
        typedef std::map<int, std::vector<sla::SupportPoint>> IdToSlaSupportPointsMap;

        // Version of the 3mf file
        unsigned int m_version;
        bool m_check_version;

        XML_Parser m_xml_parser;
        Model* m_model;
        float m_unit_factor;
        CurrentObject m_curr_object;
        IdToModelObjectMap m_objects;
        IdToAliasesMap m_objects_aliases;
        InstancesList m_instances;
        IdToGeometryMap m_geometries;
        CurrentConfig m_curr_config;
        IdToMetadataMap m_objects_metadata;
        IdToLayerHeightsProfileMap m_layer_heights_profiles;
        IdToLayerConfigRangesMap m_layer_config_ranges;
        IdToSlaSupportPointsMap m_sla_support_points;
        std::string m_curr_metadata_name;
        std::string m_curr_characters;
        std::string m_name;

    public:
        _3MF_Importer();
        ~_3MF_Importer();

        bool load_model_from_file(const std::string& filename, Model& model, DynamicPrintConfig& config, bool check_version);

    private:
        void _destroy_xml_parser();
        void _stop_xml_parser();

        bool _load_model_from_file(const std::string& filename, Model& model, DynamicPrintConfig& config);
        bool _extract_model_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_layer_heights_profile_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_layer_config_ranges_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);
        void _extract_sla_support_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat);

        void _extract_print_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, const std::string& archive_filename);
        bool _extract_model_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model);

        // handlers to parse the .model file
        void _handle_start_model_xml_element(const char* name, const char** attributes);
        void _handle_end_model_xml_element(const char* name);
        void _handle_model_xml_characters(const XML_Char* s, int len);

        // handlers to parse the MODEL_CONFIG_FILE file
        void _handle_start_config_xml_element(const char* name, const char** attributes);
        void _handle_end_config_xml_element(const char* name);

        bool _handle_start_model(const char** attributes, unsigned int num_attributes);
        bool _handle_end_model();

        bool _handle_start_resources(const char** attributes, unsigned int num_attributes);
        bool _handle_end_resources();

        bool _handle_start_object(const char** attributes, unsigned int num_attributes);
        bool _handle_end_object();

        bool _handle_start_mesh(const char** attributes, unsigned int num_attributes);
        bool _handle_end_mesh();

        bool _handle_start_vertices(const char** attributes, unsigned int num_attributes);
        bool _handle_end_vertices();

        bool _handle_start_vertex(const char** attributes, unsigned int num_attributes);
        bool _handle_end_vertex();

        bool _handle_start_triangles(const char** attributes, unsigned int num_attributes);
        bool _handle_end_triangles();

        bool _handle_start_triangle(const char** attributes, unsigned int num_attributes);
        bool _handle_end_triangle();

        bool _handle_start_components(const char** attributes, unsigned int num_attributes);
        bool _handle_end_components();

        bool _handle_start_component(const char** attributes, unsigned int num_attributes);
        bool _handle_end_component();

        bool _handle_start_build(const char** attributes, unsigned int num_attributes);
        bool _handle_end_build();

        bool _handle_start_item(const char** attributes, unsigned int num_attributes);
        bool _handle_end_item();

        bool _handle_start_metadata(const char** attributes, unsigned int num_attributes);
        bool _handle_end_metadata();

        bool _create_object_instance(int object_id, const Transform3d& transform, const bool printable, unsigned int recur_counter);

        void _apply_transform(ModelInstance& instance, const Transform3d& transform);

        bool _handle_start_config(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config();

        bool _handle_start_config_object(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_object();

        bool _handle_start_config_volume(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_volume();

        bool _handle_start_config_metadata(const char** attributes, unsigned int num_attributes);
        bool _handle_end_config_metadata();

        bool _generate_volumes(ModelObject& object, const Geometry& geometry, const ObjectMetadata::VolumeMetadataList& volumes);

        // callbacks to parse the .model file
        static void XMLCALL _handle_start_model_xml_element(void* userData, const char* name, const char** attributes);
        static void XMLCALL _handle_end_model_xml_element(void* userData, const char* name);
        static void XMLCALL _handle_model_xml_characters(void* userData, const XML_Char* s, int len);

        // callbacks to parse the MODEL_CONFIG_FILE file
        static void XMLCALL _handle_start_config_xml_element(void* userData, const char* name, const char** attributes);
        static void XMLCALL _handle_end_config_xml_element(void* userData, const char* name);
    };

    _3MF_Importer::_3MF_Importer()
        : m_version(0)
        , m_check_version(false)
        , m_xml_parser(nullptr)
        , m_model(nullptr)   
        , m_unit_factor(1.0f)
        , m_curr_metadata_name("")
        , m_curr_characters("")
        , m_name("")
    {
    }

    _3MF_Importer::~_3MF_Importer()
    {
        _destroy_xml_parser();
    }

    bool _3MF_Importer::load_model_from_file(const std::string& filename, Model& model, DynamicPrintConfig& config, bool check_version)
    {
        m_version = 0;
        m_check_version = check_version;
        m_model = &model;
        m_unit_factor = 1.0f;
        m_curr_object.reset();
        m_objects.clear();
        m_objects_aliases.clear();
        m_instances.clear();
        m_geometries.clear();
        m_curr_config.object_id = -1;
        m_curr_config.volume_id = -1;
        m_objects_metadata.clear();
        m_layer_heights_profiles.clear();
        m_layer_config_ranges.clear();
        m_sla_support_points.clear();
        m_curr_metadata_name.clear();
        m_curr_characters.clear();
        clear_errors();

        return _load_model_from_file(filename, model, config);
    }

    void _3MF_Importer::_destroy_xml_parser()
    {
        if (m_xml_parser != nullptr)
        {
            XML_ParserFree(m_xml_parser);
            m_xml_parser = nullptr;
        }
    }

    void _3MF_Importer::_stop_xml_parser()
    {
        if (m_xml_parser != nullptr)
            XML_StopParser(m_xml_parser, false);
    }

    bool _3MF_Importer::_load_model_from_file(const std::string& filename, Model& model, DynamicPrintConfig& config)
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        if (!open_zip_reader(&archive, filename)) {
            add_error("Unable to open the file");
            return false;
        }

        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);

        mz_zip_archive_file_stat stat;

        m_name = boost::filesystem::path(filename).filename().stem().string();

        // we first loop the entries to read from the archive the .model file only, in order to extract the version from it
        for (mz_uint i = 0; i < num_entries; ++i)
        {
            if (mz_zip_reader_file_stat(&archive, i, &stat))
            {
                std::string name(stat.m_filename);
                std::replace(name.begin(), name.end(), '\\', '/');

                if (boost::algorithm::istarts_with(name, MODEL_FOLDER) && boost::algorithm::iends_with(name, MODEL_EXTENSION))
                {
                    try
                    {
                        // valid model name -> extract model
                        if (!_extract_model_from_archive(archive, stat))
                        {
                            close_zip_reader(&archive);
                            add_error("Archive does not contain a valid model");
                            return false;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        // ensure the zip archive is closed and rethrow the exception
                        close_zip_reader(&archive);
                        throw std::runtime_error(e.what());
                    }
                }
            }
        }

        // we then loop again the entries to read other files stored in the archive
        for (mz_uint i = 0; i < num_entries; ++i)
        {
            if (mz_zip_reader_file_stat(&archive, i, &stat))
            {
                std::string name(stat.m_filename);
                std::replace(name.begin(), name.end(), '\\', '/');

                if (boost::algorithm::iequals(name, LAYER_HEIGHTS_PROFILE_FILE))
                {
                    // extract slic3r layer heights profile file
                    _extract_layer_heights_profile_config_from_archive(archive, stat);
                }
                if (boost::algorithm::iequals(name, LAYER_CONFIG_RANGES_FILE))
                {
                    // extract slic3r layer config ranges file
                    _extract_layer_config_ranges_from_archive(archive, stat);
                }
                else if (boost::algorithm::iequals(name, SLA_SUPPORT_POINTS_FILE))
                {
                    // extract sla support points file
                    _extract_sla_support_points_from_archive(archive, stat);
                }
                else if (boost::algorithm::iequals(name, PRINT_CONFIG_FILE))
                {
                    // extract slic3r print config file
                    _extract_print_config_from_archive(archive, stat, config, filename);
                }
                else if (boost::algorithm::iequals(name, MODEL_CONFIG_FILE))
                {
                    // extract slic3r model config file
                    if (!_extract_model_config_from_archive(archive, stat, model))
                    {
                        close_zip_reader(&archive);
                        add_error("Archive does not contain a valid model config");
                        return false;
                    }
                }
            }
        }

        close_zip_reader(&archive);

        for (const IdToModelObjectMap::value_type& object : m_objects)
        {
            ModelObject *model_object = m_model->objects[object.second];
            ObjectMetadata::VolumeMetadataList volumes;
            ObjectMetadata::VolumeMetadataList* volumes_ptr = nullptr;

            IdToGeometryMap::const_iterator obj_geometry = m_geometries.find(object.first);
            if (obj_geometry == m_geometries.end())
            {
                add_error("Unable to find object geometry");
                return false;
            }

            // m_layer_heights_profiles are indexed by a 1 based model object index.
            IdToLayerHeightsProfileMap::iterator obj_layer_heights_profile = m_layer_heights_profiles.find(object.second + 1);
            if (obj_layer_heights_profile != m_layer_heights_profiles.end())
                model_object->layer_height_profile = obj_layer_heights_profile->second;

            // m_layer_config_ranges are indexed by a 1 based model object index.
            IdToLayerConfigRangesMap::iterator obj_layer_config_ranges = m_layer_config_ranges.find(object.second + 1);
            if (obj_layer_config_ranges != m_layer_config_ranges.end())
                model_object->layer_config_ranges = obj_layer_config_ranges->second;

            // m_sla_support_points are indexed by a 1 based model object index.
            IdToSlaSupportPointsMap::iterator obj_sla_support_points = m_sla_support_points.find(object.second + 1);
            if (obj_sla_support_points != m_sla_support_points.end() && !obj_sla_support_points->second.empty()) {
                model_object->sla_support_points = obj_sla_support_points->second;
                model_object->sla_points_status = sla::PointsStatus::UserModified;
            }

            IdToMetadataMap::iterator obj_metadata = m_objects_metadata.find(object.first);
            if (obj_metadata != m_objects_metadata.end())
            {
                // config data has been found, this model was saved using slic3r pe

                // apply object's name and config data
                for (const Metadata& metadata : obj_metadata->second.metadata)
                {
                    if (metadata.key == "name")
                        model_object->name = metadata.value;
                    else
                        model_object->config.set_deserialize(metadata.key, metadata.value);
                }

                // select object's detected volumes
                volumes_ptr = &obj_metadata->second.volumes;
            }
            else
            {
                // config data not found, this model was not saved using slic3r pe

                // add the entire geometry as the single volume to generate
                volumes.emplace_back(0, (int)obj_geometry->second.triangles.size() / 3 - 1);

                // select as volumes
                volumes_ptr = &volumes;
            }

            if (!_generate_volumes(*model_object, obj_geometry->second, *volumes_ptr))
                return false;
        }

        // fixes the min z of the model if negative
        model.adjust_min_z();

        return true;
    }

    bool _3MF_Importer::_extract_model_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size == 0)
        {
            add_error("Found invalid size");
            return false;
        }

        _destroy_xml_parser();

        m_xml_parser = XML_ParserCreate(nullptr);
        if (m_xml_parser == nullptr)
        {
            add_error("Unable to create parser");
            return false;
        }

        XML_SetUserData(m_xml_parser, (void*)this);
        XML_SetElementHandler(m_xml_parser, _3MF_Importer::_handle_start_model_xml_element, _3MF_Importer::_handle_end_model_xml_element);
        XML_SetCharacterDataHandler(m_xml_parser, _3MF_Importer::_handle_model_xml_characters);

        struct CallbackData
        {
            XML_Parser& parser;
            const mz_zip_archive_file_stat& stat;

            CallbackData(XML_Parser& parser, const mz_zip_archive_file_stat& stat) : parser(parser), stat(stat) {}
        };

        CallbackData data(m_xml_parser, stat);

        mz_bool res = 0;

        try
        {
            res = mz_zip_reader_extract_file_to_callback(&archive, stat.m_filename, [](void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n)->size_t {
                CallbackData* data = (CallbackData*)pOpaque;
                if (!XML_Parse(data->parser, (const char*)pBuf, (int)n, (file_ofs + n == data->stat.m_uncomp_size) ? 1 : 0))
                {
                    char error_buf[1024];
                    ::sprintf(error_buf, "Error (%s) while parsing '%s' at line %d", XML_ErrorString(XML_GetErrorCode(data->parser)), data->stat.m_filename, (int)XML_GetCurrentLineNumber(data->parser));
                    throw std::runtime_error(error_buf);
                }

                return n;
                }, &data, 0);
        }
        catch (const version_error& e)
        {
            // rethrow the exception
            throw std::runtime_error(e.what());
        }
        catch (std::exception& e)
        {
            add_error(e.what());
            return false;
        }

        if (res == 0)
        {
            add_error("Error while extracting model data from zip archive");
            return false;
        }

        return true;
    }

    void _3MF_Importer::_extract_print_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, DynamicPrintConfig& config, const std::string& archive_filename)
    {
        if (stat.m_uncomp_size > 0)
        {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0)
            {
                add_error("Error while reading config data to buffer");
                return;
            }
            config.load_from_gcode_string(buffer.data());
        }
    }

    void _3MF_Importer::_extract_layer_heights_profile_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0)
        {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0)
            {
                add_error("Error while reading layer heights profile data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            for (const std::string& object : objects)
            {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);
                if (object_data.size() != 2)
                {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2)
                {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0)
                {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToLayerHeightsProfileMap::iterator object_item = m_layer_heights_profiles.find(object_id);
                if (object_item != m_layer_heights_profiles.end())
                {
                    add_error("Found duplicated layer heights profile");
                    continue;
                }

                std::vector<std::string> object_data_profile;
                boost::split(object_data_profile, object_data[1], boost::is_any_of(";"), boost::token_compress_off);
                if ((object_data_profile.size() <= 4) || (object_data_profile.size() % 2 != 0))
                {
                    add_error("Found invalid layer heights profile");
                    continue;
                }

                std::vector<coordf_t> profile;
                profile.reserve(object_data_profile.size());

                for (const std::string& value : object_data_profile)
                {
                    profile.push_back((coordf_t)std::atof(value.c_str()));
                }

                m_layer_heights_profiles.insert(IdToLayerHeightsProfileMap::value_type(object_id, profile));
            }
        }
    }

    void _3MF_Importer::_extract_layer_config_ranges_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0)
        {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0) {
                add_error("Error while reading layer config ranges data to buffer");
                return;
            }

            std::istringstream iss(buffer); // wrap returned xml to istringstream
            pt::ptree objects_tree;
            pt::read_xml(iss, objects_tree);

            for (const auto& object : objects_tree.get_child("objects"))
            {
                pt::ptree object_tree = object.second;
                int obj_idx = object_tree.get<int>("<xmlattr>.id", -1);
                if (obj_idx <= 0) {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToLayerConfigRangesMap::iterator object_item = m_layer_config_ranges.find(obj_idx);
                if (object_item != m_layer_config_ranges.end()) {
                    add_error("Found duplicated layer config range");
                    continue;
                }

                t_layer_config_ranges config_ranges;

                for (const auto& range : object_tree)
                {
                    if (range.first != "range")
                        continue;
                    pt::ptree range_tree = range.second;
                    double min_z = range_tree.get<double>("<xmlattr>.min_z");
                    double max_z = range_tree.get<double>("<xmlattr>.max_z");

                    // get Z range information
                    DynamicPrintConfig& config = config_ranges[{ min_z, max_z }];

                    for (const auto& option : range_tree)
                    {
                        if (option.first != "option")
                            continue;
                        std::string opt_key = option.second.get<std::string>("<xmlattr>.opt_key");
                        std::string value = option.second.data();

                        config.set_deserialize(opt_key, value);
                    }
                }

                if (!config_ranges.empty())
                    m_layer_config_ranges.insert(IdToLayerConfigRangesMap::value_type(obj_idx, config_ranges));
            }
        }
    }

    void _3MF_Importer::_extract_sla_support_points_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
    {
        if (stat.m_uncomp_size > 0)
        {
            std::string buffer((size_t)stat.m_uncomp_size, 0);
            mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
            if (res == 0)
            {
                add_error("Error while reading sla support points data to buffer");
                return;
            }

            if (buffer.back() == '\n')
                buffer.pop_back();

            std::vector<std::string> objects;
            boost::split(objects, buffer, boost::is_any_of("\n"), boost::token_compress_off);

            // Info on format versioning - see 3mf.hpp
            int version = 0;
            if (!objects.empty() && objects[0].find("support_points_format_version=") != std::string::npos) {
                objects[0].erase(objects[0].begin(), objects[0].begin() + 30); // removes the string
                version = std::stoi(objects[0]);
                objects.erase(objects.begin()); // pop the header
            }

            for (const std::string& object : objects)
            {
                std::vector<std::string> object_data;
                boost::split(object_data, object, boost::is_any_of("|"), boost::token_compress_off);

                if (object_data.size() != 2)
                {
                    add_error("Error while reading object data");
                    continue;
                }

                std::vector<std::string> object_data_id;
                boost::split(object_data_id, object_data[0], boost::is_any_of("="), boost::token_compress_off);
                if (object_data_id.size() != 2)
                {
                    add_error("Error while reading object id");
                    continue;
                }

                int object_id = std::atoi(object_data_id[1].c_str());
                if (object_id == 0)
                {
                    add_error("Found invalid object id");
                    continue;
                }

                IdToSlaSupportPointsMap::iterator object_item = m_sla_support_points.find(object_id);
                if (object_item != m_sla_support_points.end())
                {
                    add_error("Found duplicated SLA support points");
                    continue;
                }

                std::vector<std::string> object_data_points;
                boost::split(object_data_points, object_data[1], boost::is_any_of(" "), boost::token_compress_off);

                std::vector<sla::SupportPoint> sla_support_points;

                if (version == 0) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=3)
                    sla_support_points.emplace_back(float(std::atof(object_data_points[i+0].c_str())),
                                                    float(std::atof(object_data_points[i+1].c_str())),
													float(std::atof(object_data_points[i+2].c_str())),
                                                    0.4f,
                                                    false);
                }
                if (version == 1) {
                    for (unsigned int i=0; i<object_data_points.size(); i+=5)
                    sla_support_points.emplace_back(float(std::atof(object_data_points[i+0].c_str())),
                                                    float(std::atof(object_data_points[i+1].c_str())),
                                                    float(std::atof(object_data_points[i+2].c_str())),
                                                    float(std::atof(object_data_points[i+3].c_str())),
													//FIXME storing boolean as 0 / 1 and importing it as float.
                                                    std::abs(std::atof(object_data_points[i+4].c_str()) - 1.) < EPSILON);
                }

                if (!sla_support_points.empty())
                    m_sla_support_points.insert(IdToSlaSupportPointsMap::value_type(object_id, sla_support_points));
            }
        }
    }


    bool _3MF_Importer::_extract_model_config_from_archive(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat, Model& model)
    {
        if (stat.m_uncomp_size == 0)
        {
            add_error("Found invalid size");
            return false;
        }

        _destroy_xml_parser();

        m_xml_parser = XML_ParserCreate(nullptr);
        if (m_xml_parser == nullptr)
        {
            add_error("Unable to create parser");
            return false;
        }

        XML_SetUserData(m_xml_parser, (void*)this);
        XML_SetElementHandler(m_xml_parser, _3MF_Importer::_handle_start_config_xml_element, _3MF_Importer::_handle_end_config_xml_element);

        void* parser_buffer = XML_GetBuffer(m_xml_parser, (int)stat.m_uncomp_size);
        if (parser_buffer == nullptr)
        {
            add_error("Unable to create buffer");
            return false;
        }

        mz_bool res = mz_zip_reader_extract_file_to_mem(&archive, stat.m_filename, parser_buffer, (size_t)stat.m_uncomp_size, 0);
        if (res == 0)
        {
            add_error("Error while reading config data to buffer");
            return false;
        }

        if (!XML_ParseBuffer(m_xml_parser, (int)stat.m_uncomp_size, 1))
        {
            char error_buf[1024];
            ::sprintf(error_buf, "Error (%s) while parsing xml file at line %d", XML_ErrorString(XML_GetErrorCode(m_xml_parser)), (int)XML_GetCurrentLineNumber(m_xml_parser));
            add_error(error_buf);
            return false;
        }

        return true;
    }

    void _3MF_Importer::_handle_start_model_xml_element(const char* name, const char** attributes)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(m_xml_parser);

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_start_model(attributes, num_attributes);
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_start_resources(attributes, num_attributes);
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_start_object(attributes, num_attributes);
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_start_mesh(attributes, num_attributes);
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_start_vertices(attributes, num_attributes);
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_start_vertex(attributes, num_attributes);
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_start_triangles(attributes, num_attributes);
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_start_triangle(attributes, num_attributes);
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_start_components(attributes, num_attributes);
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_start_component(attributes, num_attributes);
        else if (::strcmp(BUILD_TAG, name) == 0)
            res = _handle_start_build(attributes, num_attributes);
        else if (::strcmp(ITEM_TAG, name) == 0)
            res = _handle_start_item(attributes, num_attributes);
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_start_metadata(attributes, num_attributes);

        if (!res)
            _stop_xml_parser();
    }

    void _3MF_Importer::_handle_end_model_xml_element(const char* name)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;

        if (::strcmp(MODEL_TAG, name) == 0)
            res = _handle_end_model();
        else if (::strcmp(RESOURCES_TAG, name) == 0)
            res = _handle_end_resources();
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_end_object();
        else if (::strcmp(MESH_TAG, name) == 0)
            res = _handle_end_mesh();
        else if (::strcmp(VERTICES_TAG, name) == 0)
            res = _handle_end_vertices();
        else if (::strcmp(VERTEX_TAG, name) == 0)
            res = _handle_end_vertex();
        else if (::strcmp(TRIANGLES_TAG, name) == 0)
            res = _handle_end_triangles();
        else if (::strcmp(TRIANGLE_TAG, name) == 0)
            res = _handle_end_triangle();
        else if (::strcmp(COMPONENTS_TAG, name) == 0)
            res = _handle_end_components();
        else if (::strcmp(COMPONENT_TAG, name) == 0)
            res = _handle_end_component();
        else if (::strcmp(BUILD_TAG, name) == 0)
            res = _handle_end_build();
        else if (::strcmp(ITEM_TAG, name) == 0)
            res = _handle_end_item();
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_end_metadata();

        if (!res)
            _stop_xml_parser();
    }

    void _3MF_Importer::_handle_model_xml_characters(const XML_Char* s, int len)
    {
        m_curr_characters.append(s, len);
    }

    void _3MF_Importer::_handle_start_config_xml_element(const char* name, const char** attributes)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;
        unsigned int num_attributes = (unsigned int)XML_GetSpecifiedAttributeCount(m_xml_parser);

        if (::strcmp(CONFIG_TAG, name) == 0)
            res = _handle_start_config(attributes, num_attributes);
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_start_config_object(attributes, num_attributes);
        else if (::strcmp(VOLUME_TAG, name) == 0)
            res = _handle_start_config_volume(attributes, num_attributes);
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_start_config_metadata(attributes, num_attributes);

        if (!res)
            _stop_xml_parser();
    }

    void _3MF_Importer::_handle_end_config_xml_element(const char* name)
    {
        if (m_xml_parser == nullptr)
            return;

        bool res = true;

        if (::strcmp(CONFIG_TAG, name) == 0)
            res = _handle_end_config();
        else if (::strcmp(OBJECT_TAG, name) == 0)
            res = _handle_end_config_object();
        else if (::strcmp(VOLUME_TAG, name) == 0)
            res = _handle_end_config_volume();
        else if (::strcmp(METADATA_TAG, name) == 0)
            res = _handle_end_config_metadata();

        if (!res)
            _stop_xml_parser();
    }

    bool _3MF_Importer::_handle_start_model(const char** attributes, unsigned int num_attributes)
    {
        m_unit_factor = get_unit_factor(get_attribute_value_string(attributes, num_attributes, UNIT_ATTR));
        return true;
    }

    bool _3MF_Importer::_handle_end_model()
    {
        // deletes all non-built or non-instanced objects
        for (const IdToModelObjectMap::value_type& object : m_objects)
        {
            ModelObject *model_object = m_model->objects[object.second];
            if ((model_object != nullptr) && (model_object->instances.size() == 0))
                m_model->delete_object(model_object);
        }

        // applies instances' matrices
        for (Instance& instance : m_instances)
        {
            if (instance.instance != nullptr)
            {
                ModelObject* object = instance.instance->get_object();
                if (object != nullptr)
                {
                    // apply the transform to the instance
                    _apply_transform(*instance.instance, instance.transform);
                }
            }
        }

        return true;
    }

    bool _3MF_Importer::_handle_start_resources(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_end_resources()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_object(const char** attributes, unsigned int num_attributes)
    {
        // reset current data
        m_curr_object.reset();

        if (is_valid_object_type(get_attribute_value_string(attributes, num_attributes, TYPE_ATTR)))
        {
            // create new object (it may be removed later if no instances are generated from it)
            m_curr_object.model_object_idx = (int)m_model->objects.size();
            m_curr_object.object = m_model->add_object();
            if (m_curr_object.object == nullptr)
            {
                add_error("Unable to create object");
                return false;
            }

            // set object data
            m_curr_object.object->name = get_attribute_value_string(attributes, num_attributes, NAME_ATTR);
            if (m_curr_object.object->name.empty())
                m_curr_object.object->name = m_name + "_" + std::to_string(m_model->objects.size());

            m_curr_object.id = get_attribute_value_int(attributes, num_attributes, ID_ATTR);
        }

        return true;
    }

    bool _3MF_Importer::_handle_end_object()
    {
        if (m_curr_object.object != nullptr)
        {
            if (m_curr_object.geometry.empty())
            {
                // no geometry defined
                // remove the object from the model
                m_model->delete_object(m_curr_object.object);

                if (m_curr_object.components.empty())
                {
                    // no components defined -> invalid object, delete it
                    IdToModelObjectMap::iterator object_item = m_objects.find(m_curr_object.id);
                    if (object_item != m_objects.end())
                        m_objects.erase(object_item);

                    IdToAliasesMap::iterator alias_item = m_objects_aliases.find(m_curr_object.id);
                    if (alias_item != m_objects_aliases.end())
                        m_objects_aliases.erase(alias_item);
                }
                else
                    // adds components to aliases
                    m_objects_aliases.insert(IdToAliasesMap::value_type(m_curr_object.id, m_curr_object.components));
            }
            else
            {
                // geometry defined, store it for later use
                m_geometries.insert(IdToGeometryMap::value_type(m_curr_object.id, std::move(m_curr_object.geometry)));

                // stores the object for later use
                if (m_objects.find(m_curr_object.id) == m_objects.end())
                {
                    m_objects.insert(IdToModelObjectMap::value_type(m_curr_object.id, m_curr_object.model_object_idx));
                    m_objects_aliases.insert(IdToAliasesMap::value_type(m_curr_object.id, ComponentsList(1, Component(m_curr_object.id)))); // aliases itself
                }
                else
                {
                    add_error("Found object with duplicate id");
                    return false;
                }
            }
        }

        return true;
    }

    bool _3MF_Importer::_handle_start_mesh(const char** attributes, unsigned int num_attributes)
    {
        // reset current geometry
        m_curr_object.geometry.reset();
        return true;
    }

    bool _3MF_Importer::_handle_end_mesh()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_vertices(const char** attributes, unsigned int num_attributes)
    {
        // reset current vertices
        m_curr_object.geometry.vertices.clear();
        return true;
    }

    bool _3MF_Importer::_handle_end_vertices()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_vertex(const char** attributes, unsigned int num_attributes)
    {
        // appends the vertex coordinates
        // missing values are set equal to ZERO
        m_curr_object.geometry.vertices.push_back(m_unit_factor * get_attribute_value_float(attributes, num_attributes, X_ATTR));
        m_curr_object.geometry.vertices.push_back(m_unit_factor * get_attribute_value_float(attributes, num_attributes, Y_ATTR));
        m_curr_object.geometry.vertices.push_back(m_unit_factor * get_attribute_value_float(attributes, num_attributes, Z_ATTR));
        return true;
    }

    bool _3MF_Importer::_handle_end_vertex()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_triangles(const char** attributes, unsigned int num_attributes)
    {
        // reset current triangles
        m_curr_object.geometry.triangles.clear();
        return true;
    }

    bool _3MF_Importer::_handle_end_triangles()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_triangle(const char** attributes, unsigned int num_attributes)
    {
        // we are ignoring the following attributes:
        // p1
        // p2
        // p3
        // pid
        // see specifications

        // appends the triangle's vertices indices
        // missing values are set equal to ZERO
        m_curr_object.geometry.triangles.push_back((unsigned int)get_attribute_value_int(attributes, num_attributes, V1_ATTR));
        m_curr_object.geometry.triangles.push_back((unsigned int)get_attribute_value_int(attributes, num_attributes, V2_ATTR));
        m_curr_object.geometry.triangles.push_back((unsigned int)get_attribute_value_int(attributes, num_attributes, V3_ATTR));
        return true;
    }

    bool _3MF_Importer::_handle_end_triangle()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_components(const char** attributes, unsigned int num_attributes)
    {
        // reset current components
        m_curr_object.components.clear();
        return true;
    }

    bool _3MF_Importer::_handle_end_components()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_component(const char** attributes, unsigned int num_attributes)
    {
        int object_id = get_attribute_value_int(attributes, num_attributes, OBJECTID_ATTR);
        Transform3d transform = get_transform_from_3mf_specs_string(get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));

        IdToModelObjectMap::iterator object_item = m_objects.find(object_id);
        if (object_item == m_objects.end())
        {
            IdToAliasesMap::iterator alias_item = m_objects_aliases.find(object_id);
            if (alias_item == m_objects_aliases.end())
            {
                add_error("Found component with invalid object id");
                return false;
            }
        }

        m_curr_object.components.emplace_back(object_id, transform);

        return true;
    }

    bool _3MF_Importer::_handle_end_component()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_build(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_end_build()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_item(const char** attributes, unsigned int num_attributes)
    {
        // we are ignoring the following attributes
        // thumbnail
        // partnumber
        // pid
        // pindex
        // see specifications

        int object_id = get_attribute_value_int(attributes, num_attributes, OBJECTID_ATTR);
        Transform3d transform = get_transform_from_3mf_specs_string(get_attribute_value_string(attributes, num_attributes, TRANSFORM_ATTR));
        int printable = get_attribute_value_bool(attributes, num_attributes, PRINTABLE_ATTR);

        return _create_object_instance(object_id, transform, printable, 1);
    }

    bool _3MF_Importer::_handle_end_item()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_metadata(const char** attributes, unsigned int num_attributes)
    {
        m_curr_characters.clear();

        std::string name = get_attribute_value_string(attributes, num_attributes, NAME_ATTR);
        if (!name.empty())
            m_curr_metadata_name = name;

        return true;
    }

    bool _3MF_Importer::_handle_end_metadata()
    {
        if (m_curr_metadata_name == SLIC3RPE_3MF_VERSION)
        {
            m_version = (unsigned int)atoi(m_curr_characters.c_str());

            if (m_check_version && (m_version > VERSION_3MF))
            {
                std::string msg = _(L("The selected 3mf file has been saved with a newer version of " + std::string(SLIC3R_APP_NAME) + " and is not compatible."));
                throw version_error(msg.c_str());
            }
        }

        return true;
    }

    bool _3MF_Importer::_create_object_instance(int object_id, const Transform3d& transform, const bool printable, unsigned int recur_counter)
    {
        static const unsigned int MAX_RECURSIONS = 10;

        // escape from circular aliasing
        if (recur_counter > MAX_RECURSIONS)
        {
            add_error("Too many recursions");
            return false;
        }

        IdToAliasesMap::iterator it = m_objects_aliases.find(object_id);
        if (it == m_objects_aliases.end())
        {
            add_error("Found item with invalid object id");
            return false;
        }

        if ((it->second.size() == 1) && (it->second[0].object_id == object_id))
        {
            // aliasing to itself

            IdToModelObjectMap::iterator object_item = m_objects.find(object_id);
            if ((object_item == m_objects.end()) || (object_item->second == -1))
            {
                add_error("Found invalid object");
                return false;
            }
            else
            {
                ModelInstance* instance = m_model->objects[object_item->second]->add_instance();
                if (instance == nullptr)
                {
                    add_error("Unable to add object instance");
                    return false;
                }
                instance->printable = printable;

                m_instances.emplace_back(instance, transform);
            }
        }
        else
        {
            // recursively process nested components
            for (const Component& component : it->second)
            {
                if (!_create_object_instance(component.object_id, transform * component.transform, printable, recur_counter + 1))
                    return false;
            }
        }

        return true;
    }

    void _3MF_Importer::_apply_transform(ModelInstance& instance, const Transform3d& transform)
    {
        Slic3r::Geometry::Transformation t(transform);
        // invalid scale value, return
        if (!t.get_scaling_factor().all())
            return;

        instance.set_transformation(t);
    }

    bool _3MF_Importer::_handle_start_config(const char** attributes, unsigned int num_attributes)
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_end_config()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_config_object(const char** attributes, unsigned int num_attributes)
    {
        int object_id = get_attribute_value_int(attributes, num_attributes, ID_ATTR);
        IdToMetadataMap::iterator object_item = m_objects_metadata.find(object_id);
        if (object_item != m_objects_metadata.end())
        {
            add_error("Found duplicated object id");
            return false;
        }

        m_objects_metadata.insert(IdToMetadataMap::value_type(object_id, ObjectMetadata()));
        m_curr_config.object_id = object_id;
        return true;
    }

    bool _3MF_Importer::_handle_end_config_object()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_config_volume(const char** attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end())
        {
            add_error("Cannot assign volume to a valid object");
            return false;
        }

        m_curr_config.volume_id = (int)object->second.volumes.size();

        unsigned int first_triangle_id = (unsigned int)get_attribute_value_int(attributes, num_attributes, FIRST_TRIANGLE_ID_ATTR);
        unsigned int last_triangle_id = (unsigned int)get_attribute_value_int(attributes, num_attributes, LAST_TRIANGLE_ID_ATTR);

        object->second.volumes.emplace_back(first_triangle_id, last_triangle_id);
        return true;
    }

    bool _3MF_Importer::_handle_end_config_volume()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_handle_start_config_metadata(const char** attributes, unsigned int num_attributes)
    {
        IdToMetadataMap::iterator object = m_objects_metadata.find(m_curr_config.object_id);
        if (object == m_objects_metadata.end())
        {
            add_error("Cannot assign metadata to valid object id");
            return false;
        }

        std::string type = get_attribute_value_string(attributes, num_attributes, TYPE_ATTR);
        std::string key = get_attribute_value_string(attributes, num_attributes, KEY_ATTR);
        std::string value = get_attribute_value_string(attributes, num_attributes, VALUE_ATTR);

        if (type == OBJECT_TYPE)
            object->second.metadata.emplace_back(key, value);
        else if (type == VOLUME_TYPE)
        {
            if (size_t(m_curr_config.volume_id) < object->second.volumes.size())
                object->second.volumes[m_curr_config.volume_id].metadata.emplace_back(key, value);
        }
        else
        {
            add_error("Found invalid metadata type");
            return false;
        }

        return true;
    }

    bool _3MF_Importer::_handle_end_config_metadata()
    {
        // do nothing
        return true;
    }

    bool _3MF_Importer::_generate_volumes(ModelObject& object, const Geometry& geometry, const ObjectMetadata::VolumeMetadataList& volumes)
    {
        if (!object.volumes.empty())
        {
            add_error("Found invalid volumes count");
            return false;
        }

        unsigned int geo_tri_count = (unsigned int)geometry.triangles.size() / 3;

        for (const ObjectMetadata::VolumeMetadata& volume_data : volumes)
        {
            if ((geo_tri_count <= volume_data.first_triangle_id) || (geo_tri_count <= volume_data.last_triangle_id) || (volume_data.last_triangle_id < volume_data.first_triangle_id))
            {
                add_error("Found invalid triangle id");
                return false;
            }

            Slic3r::Geometry::Transformation transform;
            if (m_version > 1)
            {
                // extract the volume transformation from the volume's metadata, if present
                for (const Metadata& metadata : volume_data.metadata)
                {
                    if (metadata.key == MATRIX_KEY)
                    {
                        transform.set_from_string(metadata.value);
                        break;
                    }
                }
            }
            Transform3d inv_matrix = transform.get_matrix().inverse();

            // splits volume out of imported geometry
			TriangleMesh triangle_mesh;
            stl_file    &stl             = triangle_mesh.stl;
			unsigned int triangles_count = volume_data.last_triangle_id - volume_data.first_triangle_id + 1;
			stl.stats.type = inmemory;
            stl.stats.number_of_facets = (uint32_t)triangles_count;
            stl.stats.original_num_facets = (int)stl.stats.number_of_facets;
            stl_allocate(&stl);

            unsigned int src_start_id = volume_data.first_triangle_id * 3;

            for (unsigned int i = 0; i < triangles_count; ++i)
            {
                unsigned int ii = i * 3;
                stl_facet& facet = stl.facet_start[i];
                for (unsigned int v = 0; v < 3; ++v)
                {
                    unsigned int tri_id = geometry.triangles[src_start_id + ii + v] * 3;
                    Vec3f vertex(geometry.vertices[tri_id + 0], geometry.vertices[tri_id + 1], geometry.vertices[tri_id + 2]);
                    if (m_version > 1)
                        // revert the vertices to the original mesh reference system
                        vertex = (inv_matrix * vertex.cast<double>()).cast<float>();
                    ::memcpy(facet.vertex[v].data(), (const void*)vertex.data(), 3 * sizeof(float));
                }
            }

			stl_get_size(&stl);
			triangle_mesh.repair();

			ModelVolume* volume = object.add_volume(std::move(triangle_mesh));
            // apply the volume matrix taken from the metadata, if present
            if (m_version > 1)
                volume->set_transformation(transform);
            volume->calculate_convex_hull();

            // apply the remaining volume's metadata
            for (const Metadata& metadata : volume_data.metadata)
            {
                if (metadata.key == NAME_KEY)
                    volume->name = metadata.value;
                else if ((metadata.key == MODIFIER_KEY) && (metadata.value == "1"))
					volume->set_type(ModelVolumeType::PARAMETER_MODIFIER);
                else if (metadata.key == VOLUME_TYPE_KEY)
                    volume->set_type(ModelVolume::type_from_string(metadata.value));
                else if (metadata.key == SOURCE_FILE_KEY)
                    volume->source.input_file = metadata.value;
                else if (metadata.key == SOURCE_OBJECT_ID_KEY)
                    volume->source.object_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_VOLUME_ID_KEY)
                    volume->source.volume_idx = ::atoi(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_X_KEY)
                    volume->source.mesh_offset(0) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Y_KEY)
                    volume->source.mesh_offset(1) = ::atof(metadata.value.c_str());
                else if (metadata.key == SOURCE_OFFSET_Z_KEY)
                    volume->source.mesh_offset(2) = ::atof(metadata.value.c_str());
                else
                    volume->config.set_deserialize(metadata.key, metadata.value);
            }
        }

        return true;
    }

    void XMLCALL _3MF_Importer::_handle_start_model_xml_element(void* userData, const char* name, const char** attributes)
    {
        _3MF_Importer* importer = (_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_start_model_xml_element(name, attributes);
    }

    void XMLCALL _3MF_Importer::_handle_end_model_xml_element(void* userData, const char* name)
    {
        _3MF_Importer* importer = (_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_end_model_xml_element(name);
    }

    void XMLCALL _3MF_Importer::_handle_model_xml_characters(void* userData, const XML_Char* s, int len)
    {
        _3MF_Importer* importer = (_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_model_xml_characters(s, len);
    }

    void XMLCALL _3MF_Importer::_handle_start_config_xml_element(void* userData, const char* name, const char** attributes)
    {
        _3MF_Importer* importer = (_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_start_config_xml_element(name, attributes);
    }
    
    void XMLCALL _3MF_Importer::_handle_end_config_xml_element(void* userData, const char* name)
    {
        _3MF_Importer* importer = (_3MF_Importer*)userData;
        if (importer != nullptr)
            importer->_handle_end_config_xml_element(name);
    }

    class _3MF_Exporter : public _3MF_Base
    {
        struct BuildItem
        {
            unsigned int id;
            Transform3d transform;
            bool printable;

            BuildItem(unsigned int id, const Transform3d& transform, const bool printable)
                : id(id)
                , transform(transform)
                , printable(printable)
            {
            }
        };

        struct Offsets
        {
            unsigned int first_vertex_id;
            unsigned int first_triangle_id;
            unsigned int last_triangle_id;

            Offsets(unsigned int first_vertex_id)
                : first_vertex_id(first_vertex_id)
                , first_triangle_id(-1)
                , last_triangle_id(-1)
            {
            }
        };

        typedef std::map<const ModelVolume*, Offsets> VolumeToOffsetsMap;

        struct ObjectData
        {
            ModelObject* object;
            VolumeToOffsetsMap volumes_offsets;

            explicit ObjectData(ModelObject* object)
                : object(object)
            {
            }
        };

        typedef std::vector<BuildItem> BuildItemsList;
        typedef std::map<int, ObjectData> IdToObjectDataMap;

    public:
#if ENABLE_THUMBNAIL_GENERATOR
        bool save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config, const ThumbnailData* thumbnail_data = nullptr);
#else
        bool save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config);
#endif // ENABLE_THUMBNAIL_GENERATOR

    private:
#if ENABLE_THUMBNAIL_GENERATOR
        bool _save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config, const ThumbnailData* thumbnail_data);
#else
        bool _save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config);
#endif // ENABLE_THUMBNAIL_GENERATOR
        bool _add_content_types_file_to_archive(mz_zip_archive& archive);
#if ENABLE_THUMBNAIL_GENERATOR
        bool _add_thumbnail_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR
        bool _add_relationships_file_to_archive(mz_zip_archive& archive);
        bool _add_model_file_to_archive(mz_zip_archive& archive, const Model& model, IdToObjectDataMap &objects_data);
        bool _add_object_to_model_stream(std::stringstream& stream, unsigned int& object_id, ModelObject& object, BuildItemsList& build_items, VolumeToOffsetsMap& volumes_offsets);
        bool _add_mesh_to_object_stream(std::stringstream& stream, ModelObject& object, VolumeToOffsetsMap& volumes_offsets);
        bool _add_build_to_model_stream(std::stringstream& stream, const BuildItemsList& build_items);
        bool _add_layer_height_profile_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_layer_config_ranges_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_sla_support_points_file_to_archive(mz_zip_archive& archive, Model& model);
        bool _add_print_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config);
        bool _add_model_config_file_to_archive(mz_zip_archive& archive, const Model& model, const IdToObjectDataMap &objects_data);
    };

#if ENABLE_THUMBNAIL_GENERATOR
    bool _3MF_Exporter::save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config, const ThumbnailData* thumbnail_data)
    {
        clear_errors();
        return _save_model_to_file(filename, model, config, thumbnail_data);
    }
#else
    bool _3MF_Exporter::save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config)
    {
        clear_errors();
        return _save_model_to_file(filename, model, config);
    }
#endif // ENABLE_THUMBNAIL_GENERATOR

#if ENABLE_THUMBNAIL_GENERATOR
    bool _3MF_Exporter::_save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config, const ThumbnailData* thumbnail_data)
#else
    bool _3MF_Exporter::_save_model_to_file(const std::string& filename, Model& model, const DynamicPrintConfig* config)
#endif // ENABLE_THUMBNAIL_GENERATOR
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        if (!open_zip_writer(&archive, filename)) {
            add_error("Unable to open the file");
            return false;
        }

        // Adds content types file ("[Content_Types].xml";).
        // The content of this file is the same for each PrusaSlicer 3mf.
        if (!_add_content_types_file_to_archive(archive))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

#if ENABLE_THUMBNAIL_GENERATOR
        if ((thumbnail_data != nullptr) && thumbnail_data->is_valid())
        {
            // Adds the file Metadata/thumbnail.png.
            if (!_add_thumbnail_file_to_archive(archive, *thumbnail_data))
            {
                close_zip_writer(&archive);
                boost::filesystem::remove(filename);
                return false;
            }
        }
#endif // ENABLE_THUMBNAIL_GENERATOR

        // Adds relationships file ("_rels/.rels"). 
        // The content of this file is the same for each PrusaSlicer 3mf.
        // The relationshis file contains a reference to the geometry file "3D/3dmodel.model", the name was chosen to be compatible with CURA.
        if (!_add_relationships_file_to_archive(archive))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        // Adds model file ("3D/3dmodel.model").
        // This is the one and only file that contains all the geometry (vertices and triangles) of all ModelVolumes.
        IdToObjectDataMap objects_data;
        if (!_add_model_file_to_archive(archive, model, objects_data))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        // Adds layer height profile file ("Metadata/Slic3r_PE_layer_heights_profile.txt").
        // All layer height profiles of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
        // The index differes from the index of an object ID of an object instance of a 3MF file!
        if (!_add_layer_height_profile_file_to_archive(archive, model))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        // Adds layer config ranges file ("Metadata/Slic3r_PE_layer_config_ranges.txt").
        // All layer height profiles of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
        // The index differes from the index of an object ID of an object instance of a 3MF file!
        if (!_add_layer_config_ranges_file_to_archive(archive, model))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        // Adds sla support points file ("Metadata/Slic3r_PE_sla_support_points.txt").
        // All  sla support points of all ModelObjects are stored here, indexed by 1 based index of the ModelObject in Model.
        // The index differes from the index of an object ID of an object instance of a 3MF file!
        if (!_add_sla_support_points_file_to_archive(archive, model))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        // Adds slic3r print config file ("Metadata/Slic3r_PE.config").
        // This file contains the content of FullPrintConfing / SLAFullPrintConfig.
        if (config != nullptr)
        {
            if (!_add_print_config_file_to_archive(archive, *config))
            {
                close_zip_writer(&archive);
                boost::filesystem::remove(filename);
                return false;
            }
        }

        // Adds slic3r model config file ("Metadata/Slic3r_PE_model.config").
        // This file contains all the attributes of all ModelObjects and their ModelVolumes (names, parameter overrides).
        // As there is just a single Indexed Triangle Set data stored per ModelObject, offsets of volumes into their respective Indexed Triangle Set data
        // is stored here as well.
        if (!_add_model_config_file_to_archive(archive, model, objects_data))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            return false;
        }

        if (!mz_zip_writer_finalize_archive(&archive))
        {
            close_zip_writer(&archive);
            boost::filesystem::remove(filename);
            add_error("Unable to finalize the archive");
            return false;
        }

        close_zip_writer(&archive);

        return true;
    }

    bool _3MF_Exporter::_add_content_types_file_to_archive(mz_zip_archive& archive)
    {
        std::stringstream stream;
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n";
        stream << " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\" />\n";
        stream << " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\" />\n";
#if ENABLE_THUMBNAIL_GENERATOR
        stream << " <Default Extension=\"png\" ContentType=\"image/png\" />\n";
#endif // ENABLE_THUMBNAIL_GENERATOR
        stream << "</Types>";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, CONTENT_TYPES_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
        {
            add_error("Unable to add content types file to archive");
            return false;
        }

        return true;
    }

#if ENABLE_THUMBNAIL_GENERATOR
    bool _3MF_Exporter::_add_thumbnail_file_to_archive(mz_zip_archive& archive, const ThumbnailData& thumbnail_data)
    {
        bool res = false;

        size_t png_size = 0;
        void* png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)thumbnail_data.pixels.data(), thumbnail_data.width, thumbnail_data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
        if (png_data != nullptr)
        {
            res = mz_zip_writer_add_mem(&archive, THUMBNAIL_FILE.c_str(), (const void*)png_data, png_size, MZ_DEFAULT_COMPRESSION);
            mz_free(png_data);
        }

        if (!res)
            add_error("Unable to add thumbnail file to archive");

        return res;
    }
#endif // ENABLE_THUMBNAIL_GENERATOR

    bool _3MF_Exporter::_add_relationships_file_to_archive(mz_zip_archive& archive)
    {
        std::stringstream stream;
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
        stream << " <Relationship Target=\"/" << MODEL_FILE << "\" Id=\"rel-1\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\" />\n";
#if ENABLE_THUMBNAIL_GENERATOR
        stream << " <Relationship Target=\"/" << THUMBNAIL_FILE << "\" Id=\"rel-2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail\" />\n";
#endif // ENABLE_THUMBNAIL_GENERATOR
        stream << "</Relationships>";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, RELATIONSHIPS_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
        {
            add_error("Unable to add relationships file to archive");
            return false;
        }

        return true;
    }

	bool _3MF_Exporter::_add_model_file_to_archive(mz_zip_archive& archive, const Model& model, IdToObjectDataMap &objects_data)
    {
        std::stringstream stream;
        // https://en.cppreference.com/w/cpp/types/numeric_limits/max_digits10
        // Conversion of a floating-point value to text and back is exact as long as at least max_digits10 were used (9 for float, 17 for double).
        // It is guaranteed to produce the same floating-point value, even though the intermediate text representation is not exact.
        // The default value of std::stream precision is 6 digits only!
		stream << std::setprecision(std::numeric_limits<float>::max_digits10);
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<" << MODEL_TAG << " unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" xmlns:slic3rpe=\"http://schemas.slic3r.org/3mf/2017/06\">\n";
        stream << " <" << METADATA_TAG << " name=\"" << SLIC3RPE_3MF_VERSION << "\">" << VERSION_3MF << "</" << METADATA_TAG << ">\n";
        stream << " <" << RESOURCES_TAG << ">\n";

        // Instance transformations, indexed by the 3MF object ID (which is a linear serialization of all instances of all ModelObjects).
        BuildItemsList build_items;

        // The object_id here is a one based identifier of the first instance of a ModelObject in the 3MF file, where
        // all the object instances of all ModelObjects are stored and indexed in a 1 based linear fashion.
        // Therefore the list of object_ids here may not be continuous.
        unsigned int object_id = 1;
        for (ModelObject* obj : model.objects)
        {
            if (obj == nullptr)
                continue;

            // Index of an object in the 3MF file corresponding to the 1st instance of a ModelObject.
            unsigned int curr_id = object_id;
            IdToObjectDataMap::iterator object_it = objects_data.insert(IdToObjectDataMap::value_type(curr_id, ObjectData(obj))).first;
            // Store geometry of all ModelVolumes contained in a single ModelObject into a single 3MF indexed triangle set object.
            // object_it->second.volumes_offsets will contain the offsets of the ModelVolumes in that single indexed triangle set.
            // object_id will be increased to point to the 1st instance of the next ModelObject.
            if (!_add_object_to_model_stream(stream, object_id, *obj, build_items, object_it->second.volumes_offsets))
            {
                add_error("Unable to add object to archive");
                return false;
            }
        }

        stream << " </" << RESOURCES_TAG << ">\n";

        // Store the transformations of all the ModelInstances of all ModelObjects, indexed in a linear fashion.
        if (!_add_build_to_model_stream(stream, build_items))
        {
            add_error("Unable to add build to archive");
            return false;
        }

        stream << "</" << MODEL_TAG << ">\n";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, MODEL_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
        {
            add_error("Unable to add model file to archive");
            return false;
        }

        return true;
    }

    bool _3MF_Exporter::_add_object_to_model_stream(std::stringstream& stream, unsigned int& object_id, ModelObject& object, BuildItemsList& build_items, VolumeToOffsetsMap& volumes_offsets)
    {
        unsigned int id = 0;
        for (const ModelInstance* instance : object.instances)
        {
			assert(instance != nullptr);
            if (instance == nullptr)
                continue;

            unsigned int instance_id = object_id + id;
            stream << "  <" << OBJECT_TAG << " id=\"" << instance_id << "\" type=\"model\">\n";

            if (id == 0)
            {
                if (!_add_mesh_to_object_stream(stream, object, volumes_offsets))
                {
                    add_error("Unable to add mesh to archive");
                    return false;
                }
            }
            else
            {
                stream << "   <" << COMPONENTS_TAG << ">\n";
                stream << "    <" << COMPONENT_TAG << " objectid=\"" << object_id << "\" />\n";
                stream << "   </" << COMPONENTS_TAG << ">\n";
            }

            Transform3d t = instance->get_matrix();
            // instance_id is just a 1 indexed index in build_items.
            assert(instance_id == build_items.size() + 1);
            build_items.emplace_back(instance_id, t, instance->printable);

            stream << "  </" << OBJECT_TAG << ">\n";

            ++id;
        }

        object_id += id;
        return true;
    }

    bool _3MF_Exporter::_add_mesh_to_object_stream(std::stringstream& stream, ModelObject& object, VolumeToOffsetsMap& volumes_offsets)
    {
        stream << "   <" << MESH_TAG << ">\n";
        stream << "    <" << VERTICES_TAG << ">\n";

        unsigned int vertices_count = 0;
        for (ModelVolume* volume : object.volumes)
        {
            if (volume == nullptr)
                continue;

			if (!volume->mesh().repaired)
				throw std::runtime_error("store_3mf() requires repair()");
			if (!volume->mesh().has_shared_vertices())
				throw std::runtime_error("store_3mf() requires shared vertices");

            volumes_offsets.insert(VolumeToOffsetsMap::value_type(volume, Offsets(vertices_count))).first;

            const indexed_triangle_set &its = volume->mesh().its;
            if (its.vertices.empty())
            {
                add_error("Found invalid mesh");
                return false;
            }

            vertices_count += (int)its.vertices.size();

            const Transform3d& matrix = volume->get_matrix();

            for (size_t i = 0; i < its.vertices.size(); ++i)
            {
                stream << "     <" << VERTEX_TAG << " ";
                Vec3f v = (matrix * its.vertices[i].cast<double>()).cast<float>();
                stream << "x=\"" << v(0) << "\" ";
                stream << "y=\"" << v(1) << "\" ";
                stream << "z=\"" << v(2) << "\" />\n";
            }
        }

        stream << "    </" << VERTICES_TAG << ">\n";
        stream << "    <" << TRIANGLES_TAG << ">\n";

        unsigned int triangles_count = 0;
        for (ModelVolume* volume : object.volumes)
        {
            if (volume == nullptr)
                continue;

            VolumeToOffsetsMap::iterator volume_it = volumes_offsets.find(volume);
            assert(volume_it != volumes_offsets.end());

            const indexed_triangle_set &its = volume->mesh().its;

            // updates triangle offsets
            volume_it->second.first_triangle_id = triangles_count;
            triangles_count += (int)its.indices.size();
            volume_it->second.last_triangle_id = triangles_count - 1;

            for (size_t i = 0; i < its.indices.size(); ++ i)
            {
                stream << "     <" << TRIANGLE_TAG << " ";
                for (int j = 0; j < 3; ++j)
                {
                    stream << "v" << j + 1 << "=\"" << its.indices[i][j] + volume_it->second.first_vertex_id << "\" ";
                }
                stream << "/>\n";
            }
        }

        stream << "    </" << TRIANGLES_TAG << ">\n";
        stream << "   </" << MESH_TAG << ">\n";

        return true;
    }

    bool _3MF_Exporter::_add_build_to_model_stream(std::stringstream& stream, const BuildItemsList& build_items)
    {
        if (build_items.size() == 0)
        {
            add_error("No build item found");
            return false;
        }

        stream << " <" << BUILD_TAG << ">\n";

        for (const BuildItem& item : build_items)
        {
            stream << "  <" << ITEM_TAG << " " << OBJECTID_ATTR << "=\"" << item.id << "\" " << TRANSFORM_ATTR << "=\"";
            for (unsigned c = 0; c < 4; ++c)
            {
                for (unsigned r = 0; r < 3; ++r)
                {
                    stream << item.transform(r, c);
                    if ((r != 2) || (c != 3))
                        stream << " ";
                }
            }
            stream << "\" " << PRINTABLE_ATTR << "=\"" << item.printable << "\" />\n";
        }

        stream << " </" << BUILD_TAG << ">\n";

        return true;
    }

    bool _3MF_Exporter::_add_layer_height_profile_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        std::string out = "";
        char buffer[1024];

        unsigned int count = 0;
        for (const ModelObject* object : model.objects)
        {
            ++count;
            const std::vector<double> &layer_height_profile = object->layer_height_profile;
            if ((layer_height_profile.size() >= 4) && ((layer_height_profile.size() % 2) == 0))
            {
                sprintf(buffer, "object_id=%d|", count);
                out += buffer;

                // Store the layer height profile as a single semicolon separated list.
                for (size_t i = 0; i < layer_height_profile.size(); ++i)
                {
                    sprintf(buffer, (i == 0) ? "%f" : ";%f", layer_height_profile[i]);
                    out += buffer;
                }
                
                out += "\n";
            }
        }

        if (!out.empty())
        {
            if (!mz_zip_writer_add_mem(&archive, LAYER_HEIGHTS_PROFILE_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
            {
                add_error("Unable to add layer heights profile file to archive");
                return false;
            }
        }

        return true;
    }

    bool _3MF_Exporter::_add_layer_config_ranges_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        std::string out = "";
        pt::ptree tree;

        unsigned int object_cnt = 0;
        for (const ModelObject* object : model.objects)
        {
            object_cnt++;
            const t_layer_config_ranges& ranges = object->layer_config_ranges;
            if (!ranges.empty())
            {
                pt::ptree& obj_tree = tree.add("objects.object","");

                obj_tree.put("<xmlattr>.id", object_cnt);

                // Store the layer config ranges.
                for (const auto& range : ranges)
                {
                    pt::ptree& range_tree = obj_tree.add("range", "");

                    // store minX and maxZ
                    range_tree.put("<xmlattr>.min_z", range.first.first);
                    range_tree.put("<xmlattr>.max_z", range.first.second);

                    // store range configuration
                    const DynamicPrintConfig& config = range.second;
                    for (const std::string& opt_key : config.keys())
                    {
                        pt::ptree& opt_tree = range_tree.add("option", config.opt_serialize(opt_key));
                        opt_tree.put("<xmlattr>.opt_key", opt_key);
                    }
                }
            }
        }

        if (!tree.empty())
        {
            std::ostringstream oss;
            boost::property_tree::write_xml(oss, tree);
            out = oss.str();

            // Post processing("beautification") of the output string for a better preview
            boost::replace_all(out, "><object",      ">\n <object");
            boost::replace_all(out, "><range",       ">\n  <range");
            boost::replace_all(out, "><option",      ">\n   <option");
            boost::replace_all(out, "></range>",     ">\n  </range>");
            boost::replace_all(out, "></object>",    ">\n </object>");
            // OR just 
            boost::replace_all(out, "><",            ">\n<"); 
        }

        if (!out.empty())
        {
            if (!mz_zip_writer_add_mem(&archive, LAYER_CONFIG_RANGES_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
            {
                add_error("Unable to add layer heights profile file to archive");
                return false;
            }
        }

        return true;
    }

    bool _3MF_Exporter::_add_sla_support_points_file_to_archive(mz_zip_archive& archive, Model& model)
    {
        std::string out = "";
        char buffer[1024];

        unsigned int count = 0;
        for (const ModelObject* object : model.objects)
        {
            ++count;
            const std::vector<sla::SupportPoint>& sla_support_points = object->sla_support_points;
            if (!sla_support_points.empty())
            {
                sprintf(buffer, "object_id=%d|", count);
                out += buffer;

                // Store the layer height profile as a single space separated list.
                for (size_t i = 0; i < sla_support_points.size(); ++i)
                {
                    sprintf(buffer, (i==0 ? "%f %f %f %f %f" : " %f %f %f %f %f"),  sla_support_points[i].pos(0), sla_support_points[i].pos(1), sla_support_points[i].pos(2), sla_support_points[i].head_front_radius, (float)sla_support_points[i].is_new_island);
                    out += buffer;
                }
                out += "\n";
            }
        }

        if (!out.empty())
        {
            // Adds version header at the beginning:
            out = std::string("support_points_format_version=") + std::to_string(support_points_format_version) + std::string("\n") + out;

            if (!mz_zip_writer_add_mem(&archive, SLA_SUPPORT_POINTS_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
            {
                add_error("Unable to add sla support points file to archive");
                return false;
            }
        }
        return true;
    }

    bool _3MF_Exporter::_add_print_config_file_to_archive(mz_zip_archive& archive, const DynamicPrintConfig &config)
    {
        char buffer[1024];
        sprintf(buffer, "; %s\n\n", header_slic3r_generated().c_str());
        std::string out = buffer;

        for (const std::string &key : config.keys())
            if (key != "compatible_printers")
                out += "; " + key + " = " + config.opt_serialize(key) + "\n";

        if (!out.empty())
        {
            if (!mz_zip_writer_add_mem(&archive, PRINT_CONFIG_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
            {
                add_error("Unable to add print config file to archive");
                return false;
            }
        }

        return true;
    }

    bool _3MF_Exporter::_add_model_config_file_to_archive(mz_zip_archive& archive, const Model& model, const IdToObjectDataMap &objects_data)
    {
        std::stringstream stream;
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        stream << "<" << CONFIG_TAG << ">\n";

        for (const IdToObjectDataMap::value_type& obj_metadata : objects_data)
        {
            const ModelObject* obj = obj_metadata.second.object;
            if (obj != nullptr)
            {
                stream << " <" << OBJECT_TAG << " id=\"" << obj_metadata.first << "\">\n";

                // stores object's name
                if (!obj->name.empty())
                    stream << "  <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << OBJECT_TYPE << "\" " << KEY_ATTR << "=\"name\" " << VALUE_ATTR << "=\"" << xml_escape(obj->name) << "\"/>\n";

                // stores object's config data
                for (const std::string& key : obj->config.keys())
                {
                    stream << "  <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << OBJECT_TYPE << "\" " << KEY_ATTR << "=\"" << key << "\" " << VALUE_ATTR << "=\"" << obj->config.opt_serialize(key) << "\"/>\n";
                }

                for (const ModelVolume* volume : obj_metadata.second.object->volumes)
                {
                    if (volume != nullptr)
                    {
                        const VolumeToOffsetsMap& offsets = obj_metadata.second.volumes_offsets;
                        VolumeToOffsetsMap::const_iterator it = offsets.find(volume);
                        if (it != offsets.end())
                        {
                            // stores volume's offsets
                            stream << "  <" << VOLUME_TAG << " ";
                            stream << FIRST_TRIANGLE_ID_ATTR << "=\"" << it->second.first_triangle_id << "\" ";
                            stream << LAST_TRIANGLE_ID_ATTR << "=\"" << it->second.last_triangle_id << "\">\n";

                            // stores volume's name
                            if (!volume->name.empty())
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << NAME_KEY << "\" " << VALUE_ATTR << "=\"" << xml_escape(volume->name) << "\"/>\n";

                            // stores volume's modifier field (legacy, to support old slicers)
                            if (volume->is_modifier())
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << MODIFIER_KEY << "\" " << VALUE_ATTR << "=\"1\"/>\n";
                            // stores volume's type (overrides the modifier field above)
                            stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << VOLUME_TYPE_KEY << "\" " << 
                                VALUE_ATTR << "=\"" << ModelVolume::type_to_string(volume->type()) << "\"/>\n";

                            // stores volume's local matrix
                            stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << MATRIX_KEY << "\" " << VALUE_ATTR << "=\"";
                            const Transform3d& matrix = volume->get_matrix();
                            for (int r = 0; r < 4; ++r)
                            {
                                for (int c = 0; c < 4; ++c)
                                {
                                    stream << matrix(r, c);
                                    if ((r != 3) || (c != 3))
                                        stream << " ";
                                }
                            }
                            stream << "\"/>\n";

                            // stores volume's source data
                            if (!volume->source.input_file.empty())
                            {
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_FILE_KEY << "\" " << VALUE_ATTR << "=\"" << xml_escape(volume->source.input_file) << "\"/>\n";
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_OBJECT_ID_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.object_idx << "\"/>\n";
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_VOLUME_ID_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.volume_idx << "\"/>\n";
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_OFFSET_X_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(0) << "\"/>\n";
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_OFFSET_Y_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(1) << "\"/>\n";
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << SOURCE_OFFSET_Z_KEY << "\" " << VALUE_ATTR << "=\"" << volume->source.mesh_offset(2) << "\"/>\n";
                            }

                            // stores volume's config data
                            for (const std::string& key : volume->config.keys())
                            {
                                stream << "   <" << METADATA_TAG << " " << TYPE_ATTR << "=\"" << VOLUME_TYPE << "\" " << KEY_ATTR << "=\"" << key << "\" " << VALUE_ATTR << "=\"" << volume->config.opt_serialize(key) << "\"/>\n";
                            }

                            stream << "  </" << VOLUME_TAG << ">\n";
                        }
                    }
                }

                stream << " </" << OBJECT_TAG << ">\n";
            }
        }

        stream << "</" << CONFIG_TAG << ">\n";

        std::string out = stream.str();

        if (!mz_zip_writer_add_mem(&archive, MODEL_CONFIG_FILE.c_str(), (const void*)out.data(), out.length(), MZ_DEFAULT_COMPRESSION))
        {
            add_error("Unable to add model config file to archive");
            return false;
        }

        return true;
    }

    bool load_3mf(const char* path, DynamicPrintConfig* config, Model* model, bool check_version)
    {
        if ((path == nullptr) || (config == nullptr) || (model == nullptr))
            return false;

        _3MF_Importer importer;
        bool res = importer.load_model_from_file(path, *model, *config, check_version);
        importer.log_errors();
        return res;
    }

#if ENABLE_THUMBNAIL_GENERATOR
    bool store_3mf(const char* path, Model* model, const DynamicPrintConfig* config, const ThumbnailData* thumbnail_data)
#else
    bool store_3mf(const char* path, Model* model, const DynamicPrintConfig* config)
#endif // ENABLE_THUMBNAIL_GENERATOR
    {
        if ((path == nullptr) || (model == nullptr))
            return false;

        _3MF_Exporter exporter;
#if ENABLE_THUMBNAIL_GENERATOR
        bool res = exporter.save_model_to_file(path, *model, config, thumbnail_data);
#else
        bool res = exporter.save_model_to_file(path, *model, config);
#endif // ENABLE_THUMBNAIL_GENERATOR

        if (!res)
            exporter.log_errors();

        return res;
    }
} // namespace Slic3r
