
/**
  ******************************************************************************
  * @file    nema_hal.c
  * @author  MCD Application Team
  * @brief   NemaGFX Interfaces and Platform Specific APIs with ThreadX support.
  *          This file provides NemaGFX functions to manage the following
  *          functionalities of the NemaGFX library:
  *           + Initialization and de-initialization functions
  *           + GPU2D Registers access operation functions
  *           + Buffer Creation, Destruction, MMAP and Flusing functions
  *           + Interrupt management functions
  *           + Memory management functions
  *           + Locking/Unlocking resources functions
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2020 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */

#include <touchgfx/hal/Config.hpp>
#include <nema_sys_defs.h>
#include <nema_core.h>

#include <assert.h>
#include <string.h>

#include <stm32n6xx_hal.h>

#include "tx_api.h"
#include "app_azure_rtos_config.h"

#include "tsi_malloc.h"

#define RING_SIZE                      1024 /* Ring Buffer Size in byte */
#define NEMAGFX_MEM_POOL_SIZE          24320 /* NemaGFX byte pool size in byte */
#define NEMAGFX_STENCIL_POOL_SIZE      389120 /* NemaGFX stencil buffer pool size in byte */

LOCATION_PRAGMA_NOLOAD("Nemagfx_Memory_Pool_Buffer")
static uint8_t nemagfx_pool_mem[NEMAGFX_MEM_POOL_SIZE] LOCATION_ATTRIBUTE_NOLOAD("Nemagfx_Memory_Pool_Buffer"); /* NemaGFX memory pool */

LOCATION_PRAGMA_NOLOAD("Nemagfx_Stencil_Buffer")
static uint8_t nemagfx_stencil_buffer_mem[NEMAGFX_STENCIL_POOL_SIZE] LOCATION_ATTRIBUTE_NOLOAD("Nemagfx_Stencil_Buffer"); /* NemaGFX stencil buffer memory */

static nema_ringbuffer_t ring_buffer_str;
volatile static int last_cl_id = -1;
extern GPU2D_HandleTypeDef hgpu2d;

TX_SEMAPHORE nema_irq_sem; /* Semaphore struct */

#if (USE_HAL_GPU2D_REGISTER_CALLBACKS == 1)
static void GPU2D_CommandListCpltCallback(GPU2D_HandleTypeDef* hgpu2d, uint32_t CmdListID)
#else /* USE_HAL_GPU2D_REGISTER_CALLBACKS = 0 */
void HAL_GPU2D_CommandListCpltCallback(GPU2D_HandleTypeDef* hgpu2d, uint32_t CmdListID)
#endif /* USE_HAL_GPU2D_REGISTER_CALLBACKS = 1 */
{
    /* Prevent unused argument(s) compilation warning */
    UNUSED(hgpu2d);

    last_cl_id = CmdListID;

    /* Return a token back to a semaphore */
    tx_semaphore_put(&nema_irq_sem);
}

void HAL_GPU2D_ErrorCallback(GPU2D_HandleTypeDef *hgpu2d)
{
    uint32_t val = nema_reg_read(GPU2D_SYS_INTERRUPT); /* clear the ER interrupt */
    nema_reg_write(GPU2D_SYS_INTERRUPT, val);

    /* external GPU2D cache maintenance */
    if (val & (1UL << 2))
    {
        HAL_ICACHE_Disable();
        nema_ext_hold_deassert_imm(2);
    }
    if (val & (1UL << 3))
    {
        HAL_ICACHE_Enable();
        HAL_ICACHE_Invalidate();
        nema_ext_hold_deassert_imm(3);
    }

    return;
}

void platform_disable_cache(void)
{
    nema_ext_hold_assert(2, 1);
}

void platform_invalidate_cache(void)
{
    nema_ext_hold_assert(3, 1);
}

int32_t nema_sys_init(void)
{
    int error_code = 0;

    /* Setup GPU2D Callback */
#if (USE_HAL_GPU2D_REGISTER_CALLBACKS == 1)
    /* Register Command List Comlete Callback */
    HAL_GPU2D_RegisterCommandListCpltCallback(&hgpu2d, GPU2D_CommandListCpltCallback);
#endif /* USE_HAL_GPU2D_REGISTER_CALLBACKS = 1 */

    /* Create IRQ semaphore */
    error_code = tx_semaphore_create(&nema_irq_sem, (CHAR*) "nema sema irq", 0);
    assert(error_code == TX_SUCCESS);

    /* Initialise Mem Space */
    error_code = tsi_malloc_init_pool_aligned(0, (void*)nemagfx_pool_mem, (uintptr_t)nemagfx_pool_mem, NEMAGFX_MEM_POOL_SIZE, 1, 8);
    assert(error_code == 0);
    error_code = tsi_malloc_init_pool_aligned(1, (void*)nemagfx_stencil_buffer_mem, (uintptr_t)nemagfx_stencil_buffer_mem, NEMAGFX_STENCIL_POOL_SIZE, 1, 8);
    assert(error_code == 0);

    /* Allocate ring_buffer memory */
    ring_buffer_str.bo = nema_buffer_create(RING_SIZE);
    assert(ring_buffer_str.bo.base_virt);

    /* Initialize Ring Buffer */
    error_code = nema_rb_init(&ring_buffer_str, 1);
    if (error_code < 0)
    {
        return error_code;
    }

    /* Reset last_cl_id counter */
    last_cl_id = 0;

    return error_code;
}

uint32_t nema_reg_read(uint32_t reg)
{
    return HAL_GPU2D_ReadRegister(&hgpu2d, reg);
}

void nema_reg_write(uint32_t reg, uint32_t value)
{
    HAL_GPU2D_WriteRegister(&hgpu2d, reg, value);
}

int nema_wait_irq(void)
{
    /* Wait indefinitely for a free semaphore */
    tx_semaphore_get(&nema_irq_sem, TX_WAIT_FOREVER);

    return 0;
}

int nema_wait_irq_cl(int cl_id)
{
    while (last_cl_id < cl_id)
    {
        (void)nema_wait_irq();
    }

    return 0;
}

int nema_wait_irq_brk(int brk_id)
{
    while (nema_reg_read(GPU2D_BREAKPOINT) == 0U)
    {
        (void)nema_wait_irq();
    }

    return 0;
}

void nema_host_free(void* ptr)
{
    tsi_free(ptr);
}

void* nema_host_malloc(unsigned size)
{
    return tsi_malloc(size);
}

nema_buffer_t nema_buffer_create(int size)
{
    nema_buffer_t bo;
    memset(&bo, 0, sizeof(bo));
    bo.base_virt = tsi_malloc(size);
    bo.base_phys = (uint32_t)bo.base_virt;
    bo.size      = size;
    assert(bo.base_virt != 0 && "Unable to allocate memory in nema_buffer_create");

    return bo;
}

nema_buffer_t nema_buffer_create_pool(int pool, int size)
{
    nema_buffer_t bo;
    memset(&bo, 0, sizeof(bo));
    bo.base_virt = tsi_malloc_pool(pool, size);
    bo.base_phys = (uint32_t)bo.base_virt;
    bo.size      = size;
    bo.fd        = 0;
    assert(bo.base_virt != 0 && "Unable to allocate memory in nema_buffer_create_pool");

    return bo;
}

void* nema_buffer_map(nema_buffer_t* bo)
{
    return bo->base_virt;
}

void nema_buffer_unmap(nema_buffer_t* bo)
{
    /* Prevent unused argument(s) compilation warning */
    UNUSED(bo);
}

void nema_buffer_destroy(nema_buffer_t* bo)
{
    if (bo->fd == -1)
    {
        return; /* Buffer weren't allocated! */
    }

    tsi_free(bo->base_virt);

    bo->base_virt = (void*)0;
    bo->base_phys = 0;
    bo->size      = 0;
    bo->fd        = -1; /* Buffer not allocated */
}

uintptr_t nema_buffer_phys(nema_buffer_t* bo)
{
    return bo->base_phys;
}

void nema_buffer_flush(nema_buffer_t* bo)
{
    /* Prevent unused argument(s) compilation warning */
    UNUSED(bo);
}

int nema_mutex_lock(int mutex_id)
{
    int retval = 0;

    /* USER CODE BEGIN nema_mutex_lock */
    /* Prevent unused argument(s) compilation warning */
    UNUSED(mutex_id);
    /* USER CODE END nema_mutex_lock */

    return retval;
}

int nema_mutex_unlock(int mutex_id)
{
    int retval = 0;

    /* USER CODE BEGIN nema_mutex_unlock */
    /* Prevent unused argument(s) compilation warning */
    UNUSED(mutex_id);
    /* USER CODE END nema_mutex_unlock */

    return retval;
}
/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
