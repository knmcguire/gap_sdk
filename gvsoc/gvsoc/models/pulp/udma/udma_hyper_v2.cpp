/*
 * Copyright (C) 2018 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */


#include "udma_impl.hpp"
#include "archi/udma/hyper/udma_hyper_v2.h"
#include "archi/utils.h"
#include "vp/itf/hyper.hpp"


#define HYPER_NB_REGS 24

#define HYPER_EXT_ADDR_CHANNEL_OFFSET     0x00
#define HYPER_EXT_CFG_CHANNEL_OFFSET      0x04
#define HYPER_MEM_CFG0_CHANNEL_OFFSET     0x08
#define HYPER_MEM_CFG1_CHANNEL_OFFSET     0x0C
#define HYPER_MEM_CFG2_CHANNEL_OFFSET     0x10
#define HYPER_MEM_CFG3_CHANNEL_OFFSET     0x14
#define HYPER_MEM_CFG4_CHANNEL_OFFSET     0x18
#define HYPER_MEM_CFG5_CHANNEL_OFFSET     0x1C
#define HYPER_MEM_CFG6_CHANNEL_OFFSET     0x20
#define HYPER_MEM_CFG7_CHANNEL_OFFSET     0x24

#define HYPER_MEM_CFG3_ACS0_OFFSET              0
#define HYPER_MEM_CFG3_TCO0_OFFSET              1
#define HYPER_MEM_CFG3_DT0_OFFSET               2
#define HYPER_MEM_CFG3_CRT0_OFFSET              3 // HyperRAM memory access 0, for register access 1
#define HYPER_MEM_CFG3_RD_MAX_LEN_EN0_OFFSET    4
#define HYPER_MEM_CFG3_WR_MAX_LEN_EN0_OFFSET    5
#define HYPER_MEM_CFG3_RDS_DELAY_ADJ_OFFSET     8

Hyper_periph_v2::Hyper_periph_v2(udma *top, int id, int itf_id) : Udma_periph(top, id)
{
  std::string itf_name = "hyper" + std::to_string(itf_id);

  top->traces.new_trace(itf_name, &trace, vp::DEBUG);

  channel0 = new Hyper_v2_rx_channel(top, this, UDMA_EVENT_ID(id) + 0, itf_name + "_rx");
  channel1 = new Hyper_v2_tx_channel(top, this, UDMA_EVENT_ID(id) + 1, itf_name + "_tx");

  this->hyper_itf.set_sync_cycle_meth(&Hyper_periph_v2::rx_sync);
  top->new_master_port(this, itf_name, &this->hyper_itf);

  this->regs = new unsigned int[HYPER_NB_REGS];

  this->pending_word_event = top->event_new(this, Hyper_periph_v2::handle_pending_word);

  this->pending_bytes = 0;
  this->next_bit_cycle = -1;
  this->state = HYPER_STATE_IDLE;

  this->rx_channel = static_cast<Hyper_v2_rx_channel *>(this->channel0);
  this->tx_channel = static_cast<Hyper_v2_tx_channel *>(this->channel1);

  this->top->new_reg(itf_name + "/timing_cfg", &this->r_timing_cfg, 0);
  this->top->new_reg(itf_name + "/clk_div", &this->r_clk_div, 0);
  //hyper_itf.set_cs_sync_meth(&Hyper_periph_v2::cs_sync);

  js::config *config = this->top->get_js_config()->get("hyper/eot_events");
  if (config)
    this->eot_event = config->get_elem(itf_id)->get_int();
  else
    this->eot_event = -1;
}
 

void Hyper_periph_v2::reset(bool active)
{
  Udma_periph::reset(active);

  if (active)
  {
    memset(this->regs, 0, sizeof(unsigned int)*HYPER_NB_REGS);
    this->pending_tx = false;
    this->pending_rx = false;
    this->current_cmd = NULL;
  }
}


void Hyper_periph_v2::rx_sync(void *__this, int data)
{
  Hyper_periph_v2 *_this = (Hyper_periph_v2 *)__this;
  (static_cast<Hyper_v2_rx_channel *>(_this->channel0))->handle_rx_data(data);
}


void Hyper_periph_v2::handle_pending_word(void *__this, vp::clock_event *event)
{
  Hyper_periph_v2 *_this = (Hyper_periph_v2 *)__this;
  uint8_t byte;
  int cs_value;
  bool send_byte = false;
  bool send_cs = false;
  bool end = false;
  uint32_t mba0 = _this->regs[(HYPER_MBA0_OFFSET - 0x20)/4];
  uint32_t mba1 = _this->regs[(HYPER_MBA1_OFFSET - 0x20)/4];
  uint32_t addr = _this->regs[(HYPER_EXT_ADDR_OFFSET - 0x20)/4];
  int cs;

  if (mba1 >= mba0)
    cs = addr >= mba1 ? 1 : 0;
  else
    cs = addr >= mba0 ? 0 : 0;


  if (_this->state == HYPER_STATE_IDLE)
  {
    if (_this->pending_bytes > 0)
    {
      _this->state = HYPER_STATE_DELAY;
      _this->delay = 72;
      _this->ca_count = 6;
      _this->ca.low_addr = ARCHI_REG_FIELD_GET(_this->regs[HYPER_EXT_ADDR_CHANNEL_OFFSET/4], 0, 3);
      _this->ca.high_addr = ARCHI_REG_FIELD_GET(_this->regs[HYPER_EXT_ADDR_CHANNEL_OFFSET/4], 3, 29);
      _this->ca.burst_type = 0;
      _this->ca.address_space = ARCHI_REG_FIELD_GET(_this->regs[HYPER_MEM_CFG3_CHANNEL_OFFSET/4], HYPER_MEM_CFG3_CRT0_OFFSET, 1);
      _this->ca.read = _this->pending_rx ? 1 : 0;

      if (_this->ca.read)
        _this->transfer_size = _this->rx_channel->current_cmd->size;
      else
        _this->transfer_size = _this->tx_channel->current_cmd->size;
    }
  }
  else if (_this->state == HYPER_STATE_DELAY)
  {
    _this->delay--;
    if (_this->delay == 0)
      _this->state = HYPER_STATE_CS;
  }
  else if (_this->state == HYPER_STATE_CS)
  {
    _this->state = HYPER_STATE_CA;
    send_cs = true;
    cs_value = 1;
  }
  else if (_this->state == HYPER_STATE_CA)
  {
    send_byte = true;
    _this->ca_count--;
    byte = _this->ca.raw[_this->ca_count];
    if (_this->ca_count == 0)
    {
      _this->state = HYPER_STATE_DATA;
    }
  }
  else if (_this->state == HYPER_STATE_DATA && _this->pending_bytes > 0)
  {
    send_byte = true;
    byte = _this->pending_word & 0xff;
    _this->pending_word >>= 8;
    _this->pending_bytes--;
    _this->transfer_size--;

    if (_this->transfer_size == 0)
    {
      _this->pending_bytes = 0;
      _this->state = HYPER_STATE_CS_OFF;
    }
    if (_this->pending_bytes == 0)
    {
      end = true;
    }
  }
  else if (_this->state == HYPER_STATE_CS_OFF)
  {
    _this->state = HYPER_STATE_IDLE;
    send_cs = true;
    cs_value = 0;
  }

  if (send_byte || send_cs)
  {
    if (!_this->hyper_itf.is_bound())
    {
      _this->top->get_trace()->warning("Trying to send to HYPER interface while it is not connected\n");
    }
    else
    {
      int div = _this->r_clk_div.data_get()*2;

      _this->next_bit_cycle = _this->top->get_periph_clock()->get_cycles() + div;
      if (send_byte)
      {
        _this->top->get_trace()->msg("Sending byte (value: 0x%x)\n", byte);
        _this->hyper_itf.sync_cycle(byte);
      }
      else
      {
        _this->top->get_trace()->msg("Updating CS (cs: %d, value: %d)\n", cs, cs_value);
        _this->hyper_itf.cs_sync(cs, cs_value);
      }
    }
  }

  if (end)
  {
    if (!_this->ca.read)
    {
      _this->pending_tx = false;
      _this->tx_channel->handle_ready_req_end(_this->pending_req);
      _this->tx_channel->handle_ready_reqs();
    }
    else
      _this->pending_rx = false;

  }

  _this->check_state();
}

void Hyper_periph_v2::check_state()
{
  if (this->pending_bytes == 0)
  {
    if (!this->tx_channel->ready_reqs->is_empty() && (this->pending_tx || !this->pending_rx))
    {
      this->pending_tx = true;
      vp::io_req *req = this->tx_channel->ready_reqs->pop();
      this->pending_req = req;
      this->pending_word = *(uint32_t *)req->get_data();
      this->pending_bytes = req->get_size();
    }
    else if (this->rx_channel->current_cmd && (this->pending_rx || !this->pending_tx))
    {
      this->pending_rx = true;
      this->pending_bytes = rx_channel->current_cmd->size;
    }
  }

  if (this->pending_bytes != 0)
  {
    if (!this->pending_word_event->is_enqueued())
    {
      int latency = 1;
      int64_t cycles = this->top->get_periph_clock()->get_cycles();
      if (this->next_bit_cycle > cycles)
        latency = this->next_bit_cycle - cycles;

      this->top->get_periph_clock()->enqueue_ext(this->pending_word_event, latency);
    }
  }
}


void Hyper_periph_v2::handle_ready_reqs()
{
  this->check_state();
}



vp::io_req_status_e Hyper_periph_v2::custom_req(vp::io_req *req, uint64_t offset)
{
  vp::io_req_status_e err = vp::IO_REQ_OK;
  uint8_t *data = req->get_data();
  uint64_t size = req->get_size();
  bool is_write = req->get_is_write();

  if (req->get_size() != 4)
    return vp::IO_REQ_INVALID;

  int reg_id = offset / 4;
  int reg_offset = offset % 4;

  if (reg_id >= HYPER_NB_REGS) return vp::IO_REQ_INVALID;

  if (!req->get_is_write())
    *(uint32_t *)(req->get_data()) = this->regs[reg_id];
  else 
    this->regs[reg_id] = *(uint32_t *)(req->get_data());


  switch (reg_id + HYPER_EXT_ADDR_OFFSET/4)
  {
    case HYPER_TIMING_CFG_OFFSET/4:
      this->r_timing_cfg.access(reg_offset, size, data, is_write);
      if (is_write)
        this->trace.msg("Setting TIMING_CFG (latency0: %d, latency1: %d, rw_recovery: %d, rwds_delay: %d, lat_autocheck: %d, cs_max: %d)\n", this->r_timing_cfg.latency0_get(), this->r_timing_cfg.latency1_get(), this->r_timing_cfg.rw_recovery_get(), this->r_timing_cfg.rwds_delay_get(), this->r_timing_cfg.additional_latency_autocheck_en_get(), this->r_timing_cfg.cs_max_get());
      break;

    case HYPER_CLK_DIV_OFFSET/4:
      this->r_clk_div.access(reg_offset, size, data, is_write);
      if (is_write)
        this->trace.msg("Setting CLK_DIV (data: %d)\n", this->r_clk_div.data_get());
      break;
  }

  return err;
}


#if 0
void Hyper_periph_v2::sync_cycle(void *__this, int data)
{
  Hyper_periph_v2 *_this = (Hyper_periph_v2 *)__this;
  (static_cast<Hyper_v2_rx_channel *>(_this->channel0))->handle_rx_bit(data);
}
#endif




Hyper_v2_tx_channel::Hyper_v2_tx_channel(udma *top, Hyper_periph_v2 *periph, int id, string name)
: Udma_tx_channel(top, id, name), periph(periph)
{
}







void Hyper_v2_tx_channel::reset(bool active)
{
  Udma_tx_channel::reset(active);
}

void Hyper_v2_tx_channel::handle_ready_reqs()
{
  this->periph->handle_ready_reqs();
}


Hyper_v2_rx_channel::Hyper_v2_rx_channel(udma *top, Hyper_periph_v2 *periph, int id, string name) : Udma_rx_channel(top, id, name), periph(periph)
{
}

void Hyper_v2_rx_channel::reset(bool active)
{
  Udma_rx_channel::reset(active);
}

void Hyper_v2_rx_channel::handle_rx_data(int data)
{
  this->push_data((uint8_t *)&data, 1);
}

void Hyper_v2_rx_channel::handle_ready()
{
  this->periph->handle_ready_reqs();
}

void Hyper_v2_rx_channel::handle_transfer_end()
{
  Udma_rx_channel::handle_transfer_end();
  this->top->trigger_event(this->periph->eot_event);
}

void Hyper_v2_tx_channel::handle_transfer_end()
{
  Udma_tx_channel::handle_transfer_end();
  this->top->trigger_event(this->periph->eot_event);
}