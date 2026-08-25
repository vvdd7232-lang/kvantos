/* ============================================================
 *  KvantOS 3.0 - minimal hand-written UEFI definitions
 *
 *  Only what boot/kvantefi.c needs, straight from the UEFI
 *  specification. All firmware callbacks use the Microsoft
 *  x86-64 ABI, hence EFIAPI on every function pointer.
 * ============================================================ */
#ifndef KVANT_EFI_H
#define KVANT_EFI_H

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef long long          i64;

typedef u64  efi_status_t;
typedef u16  efi_char16_t;
typedef void *efi_handle_t;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS             0ULL
#define EFI_ERROR_BIT           0x8000000000000000ULL
#define EFI_LOAD_ERROR          (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER   (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED         (EFI_ERROR_BIT | 3)
#define EFI_BAD_BUFFER_SIZE     (EFI_ERROR_BIT | 4)
#define EFI_BUFFER_TOO_SMALL    (EFI_ERROR_BIT | 5)
#define EFI_OUT_OF_RESOURCES    (EFI_ERROR_BIT | 9)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)
#define EFI_ERR(s)              ((s) & EFI_ERROR_BIT)

typedef struct {
    u32 data1;
    u16 data2, data3;
    u8  data4[8];
} efi_guid_t;

/* ---- memory map ---- */
typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 4,
    EfiBootServicesData = 5,
    EfiRuntimeServicesCode = 3,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15
} efi_memory_type_t;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2
} efi_allocate_type_t;

typedef struct {
    u32 type;
    u32 _pad;
    u64 phys_start;
    u64 virt_start;
    u64 pages;
    u64 attribute;
} efi_memory_descriptor_t;

/* ---- table header ---- */
typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} efi_table_header_t;

/* ---- simple text output (progress messages) ---- */
typedef struct efi_simple_text_output efi_simple_text_output_t;
struct efi_simple_text_output {
    efi_status_t (EFIAPI *reset)(efi_simple_text_output_t *self,
                                 u64 extended_verification);
    efi_status_t (EFIAPI *output_string)(efi_simple_text_output_t *self,
                                         efi_char16_t *string);
};

/* ---- files ---- */
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_DIRECTORY   0x1000000000000000ULL

typedef struct efi_file efi_file_t;
struct efi_file {
    u64 revision;
    efi_status_t (EFIAPI *open)(efi_file_t *self, efi_file_t **new_handle,
                                efi_char16_t *file_name, u64 open_mode,
                                u64 attributes);
    efi_status_t (EFIAPI *close)(efi_file_t *self);
    efi_status_t (EFIAPI *delete)(efi_file_t *self);
    efi_status_t (EFIAPI *read)(efi_file_t *self, u64 *buffer_size,
                                void *buffer);
    efi_status_t (EFIAPI *write)(efi_file_t *self, u64 *buffer_size,
                                 const void *buffer);
    efi_status_t (EFIAPI *get_position)(efi_file_t *self, u64 *position);
    efi_status_t (EFIAPI *set_position)(efi_file_t *self, u64 position);
    efi_status_t (EFIAPI *get_info)(efi_file_t *self, efi_guid_t *info_type,
                                    u64 *buffer_size, void *buffer);
    efi_status_t (EFIAPI *set_info)(efi_file_t *self, efi_guid_t *info_type,
                                    u64 buffer_size, void *buffer);
    efi_status_t (EFIAPI *flush)(efi_file_t *self);
};

typedef struct {
    u64 size;
    u64 file_size;
    u64 physical_size;
    u8  create_time[16];
    u8  last_access_time[16];
    u8  modification_time[16];
    u64 attribute;
    efi_char16_t file_name[1];   /* variable length */
} efi_file_info_t;

typedef struct efi_simple_file_system efi_simple_file_system_t;
struct efi_simple_file_system {
    u64 revision;
    efi_status_t (EFIAPI *open_volume)(efi_simple_file_system_t *self,
                                       efi_file_t **root);
};

/* ---- graphics output ---- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3
} efi_graphics_pixel_format_t;

typedef struct {
    u32 red_mask, green_mask, blue_mask, reserved_mask;
} efi_pixel_bitmask_t;

typedef struct {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;
    efi_pixel_bitmask_t pixel_information;
    u32 pixels_per_scan_line;
} efi_graphics_output_mode_info_t;

typedef struct {
    u32 max_mode;
    u32 mode;
    efi_graphics_output_mode_info_t *info;
    u64 size_of_info;
    u64 frame_buffer_base;
    u64 frame_buffer_size;
} efi_graphics_output_mode_t;

typedef struct efi_graphics_output efi_graphics_output_t;
struct efi_graphics_output {
    efi_status_t (EFIAPI *query_mode)(efi_graphics_output_t *self, u32 mode,
                                      u64 *size_of_info,
                                      efi_graphics_output_mode_info_t **info);
    efi_status_t (EFIAPI *set_mode)(efi_graphics_output_t *self, u32 mode);
    efi_status_t (EFIAPI *blt)(efi_graphics_output_t *self, void *blt_buffer,
                               u32 operation, u64 sx, u64 sy, u64 dx, u64 dy,
                               u64 width, u64 height, u64 delta);
    efi_graphics_output_mode_t *mode;
};

/* ---- loaded image ---- */
typedef struct {
    u32 revision;
    u32 _pad;
    efi_handle_t parent_handle;
    void *system_table;
    efi_handle_t device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    u32 _pad2;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    void *unload;
} efi_loaded_image_t;

/* ---- boot services ---- */
typedef struct {
    efi_table_header_t hdr;
    void *raise_tpl;                 /*  0 */
    void *restore_tpl;               /*  1 */
    efi_status_t (EFIAPI *allocate_pages)(efi_allocate_type_t type,     /* 2 */
                                          efi_memory_type_t mem_type,
                                          u64 pages, u64 *memory);
    efi_status_t (EFIAPI *free_pages)(u64 memory, u64 pages);           /* 3 */
    efi_status_t (EFIAPI *get_memory_map)(u64 *map_size,                /* 4 */
                                          efi_memory_descriptor_t *map,
                                          u64 *map_key, u64 *desc_size,
                                          u32 *desc_version);
    void *allocate_pool;             /*  5 */
    void *free_pool;                 /*  6 */
    void *create_event;              /*  7 */
    void *set_timer;                 /*  8 */
    void *wait_for_event;            /*  9 */
    void *signal_event;              /* 10 */
    void *close_event;               /* 11 */
    void *check_event;               /* 12 */
    void *install_protocol_interface;/* 13 */
    void *reinstall_protocol_interface; /* 14 */
    void *uninstall_protocol_interface; /* 15 */
    efi_status_t (EFIAPI *handle_protocol)(efi_handle_t handle,           /* 16 */
                                           efi_guid_t *protocol,
                                           void **interface);
    void *reserved;                  /* 17 */
    void *register_protocol_notify;  /* 18 */
    efi_status_t (EFIAPI *locate_handle)(u32 search_type,                 /* 19 */
                                         efi_guid_t *protocol,
                                         void *search_key,
                                         u64 *buffer_size,
                                         efi_handle_t *buffer);
    void *locate_device_path;        /* 20 */
    void *install_configuration_table; /* 21 */
    void *load_image;                /* 22 */
    void *start_image;               /* 23 */
    efi_status_t (EFIAPI *exit)(efi_handle_t image_handle,                /* 24 */
                                efi_status_t status, u64 exit_data_size,
                                efi_char16_t *exit_data);
    void *unload_image;              /* 25 */
    efi_status_t (EFIAPI *exit_boot_services)(efi_handle_t image_handle,  /* 26 */
                                              u64 map_key);
} efi_boot_services_t;

/* ---- system table ---- */
typedef struct {
    efi_table_header_t hdr;
    efi_char16_t *firmware_vendor;
    u32 firmware_revision;
    u32 _pad;
    efi_handle_t console_in_handle;
    void *con_in;
    efi_handle_t console_out_handle;
    efi_simple_text_output_t *con_out;
    efi_handle_t standard_error_handle;
    efi_simple_text_output_t *std_err;
    void *runtime_services;
    efi_boot_services_t *boot_services;
    u64 number_of_table_entries;
    void *configuration_table;
} efi_system_table_t;

/* ---- well-known GUIDs ---- */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#endif /* KVANT_EFI_H */
