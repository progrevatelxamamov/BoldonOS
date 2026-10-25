/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice, text.
 */

#ifndef EXT4_CONFIG_H_
#define EXT4_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Отключаем чтение внешних генерируемых файлов, выставляем всё жестко здесь */
#define CONFIG_USE_DEFAULT_CFG 1

/*****************************************************************************/

#define F_SET_EXT2 2
#define F_SET_EXT3 3
#define F_SET_EXT4 4

/* Включаем полный набор фич EXT4 (включая extents и metadata_csum) */
#ifndef CONFIG_EXT_FEATURE_SET_LVL
#define CONFIG_EXT_FEATURE_SET_LVL F_SET_EXT4
#endif

/*****************************************************************************/

#if CONFIG_EXT_FEATURE_SET_LVL == F_SET_EXT2
#define CONFIG_SUPPORTED_FCOM EXT2_SUPPORTED_FCOM
#define CONFIG_SUPPORTED_FINCOM (EXT2_SUPPORTED_FINCOM | EXT_FINCOM_IGNORED)
#define CONFIG_SUPPORTED_FRO_COM EXT2_SUPPORTED_FRO_COM
#elif CONFIG_EXT_FEATURE_SET_LVL == F_SET_EXT3
#define CONFIG_SUPPORTED_FCOM EXT3_SUPPORTED_FCOM
#define CONFIG_SUPPORTED_FINCOM (EXT3_SUPPORTED_FINCOM | EXT_FINCOM_IGNORED)
#define CONFIG_SUPPORTED_FRO_COM EXT3_SUPPORTED_FRO_COM
#elif CONFIG_EXT_FEATURE_SET_LVL == F_SET_EXT4
#define CONFIG_SUPPORTED_FCOM EXT4_SUPPORTED_FCOM
#define CONFIG_SUPPORTED_FINCOM (EXT4_SUPPORTED_FINCOM | EXT_FINCOM_IGNORED)
#define CONFIG_SUPPORTED_FRO_COM EXT4_SUPPORTED_FRO_COM
#endif

#define CONFIG_DIR_INDEX_ENABLE (CONFIG_SUPPORTED_FCOM & EXT4_FCOM_DIR_INDEX)
#define CONFIG_EXTENT_ENABLE (CONFIG_SUPPORTED_FINCOM & EXT4_FINCOM_EXTENTS)
#define CONFIG_META_CSUM_ENABLE (CONFIG_SUPPORTED_FRO_COM & EXT4_FRO_COM_METADATA_CSUM)

/*****************************************************************************/

/**@brief  Включаем мьютексы — ОБЯЗАТЕЛЬНО для корректной работы JBD2 журнала! */
#define CONFIG_LOCK_MUTEX_ENABLE 1

/**@brief  Включаем журналирование (JBD2) */
#define CONFIG_JOURNALING_ENABLE 1

/**@brief  Включаем расширенные атрибуты xattr */
#define CONFIG_XATTR_ENABLE 1

/**@brief  Включаем экстенты (extents) */
#define CONFIG_EXTENTS_ENABLE 1

/**@brief   Использовать свои коды ошибок. В BoldonOS лучше 0 (стандартный POSIX) */
#define CONFIG_HAVE_OWN_ERRNO 0

/**@brief   Логирование и отладка (выключите, если сыплет лишним в консоль) */
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0
#define CONFIG_HAVE_OWN_ASSERT 1

/**@brief   Статистика блочного устройства */
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0

/**@brief   Размер кэша блоков (16 или 32 ускорит чтение метаданных на ПК) */
#define CONFIG_BLOCK_DEV_CACHE_SIZE 32

/**@brief   Максимальное имя устройства и лимиты (выставили 4, как в вашем коде) */
#define CONFIG_EXT4_MAX_BLOCKDEV_NAME 32
#define CONFIG_EXT4_BLOCKDEVS_COUNT 4

#define CONFIG_EXT4_MAX_MP_NAME 32
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 4

/**@brief   Флаги открытия файлов (O_RDONLY, O_WRONLY и т.д.) */
#define CONFIG_HAVE_OWN_OFLAGS 1

/**@brief   Максимальный размер транзакции усечения */
#define CONFIG_MAX_TRUNCATE_SIZE (16ul * 1024ul * 1024ul)

/**@brief   Выравнивание доступа к памяти (0 — если x86_64, процессор сам умеет невыровненный доступ) */
#define CONFIG_UNALIGNED_ACCESS 0

/**@brief   Использовать ваш кастомный malloc из BoldonOS (поставьте 1, если нет стандартной stdlib) */
#define CONFIG_USE_USER_MALLOC 0

#ifdef __cplusplus
}
#endif

#endif /* EXT4_CONFIG_H_ */
