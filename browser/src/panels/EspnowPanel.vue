<template>
  <div class="q-gutter-y-md">
    <PanelHeading title="ESPnow" />

    <div class="text-caption" style="opacity:0.7">
      RNS over ESP-NOW on the chip's long-range PHY. Every node on the
      same 2.4&nbsp;GHz channel hears every packet. Requires WiFi to be
      up; while associated to an access point the channel is fixed by
      that AP (all nodes must share it).
    </div>

    <q-banner v-if="conflictCh" dense class="bg-orange-9 text-white rounded-borders">
      <template #avatar>
        <q-icon name="warning" />
      </template>
      ESPnow can't start: setting it to channel {{ channel }} would drop
      this device's WiFi, which is on channel {{ conflictCh }}. The
      channel is shared by WiFi and ESPnow (station and access-point
      modes alike). To run both, change your 2.4&nbsp;GHz router to
      channel {{ channel }} — or set ESPnow to channel {{ conflictCh }}.
    </q-banner>

    <SettingToggle label="Enabled" k="s.espnow.enable" />

    <SettingSelect label="WiFi channel" k="s.espnow.channel" :options="chanOptions" />
    <SettingSelect label="Long-range rate" k="s.espnow.rate" :options="rateOptions" />

    <div class="section-heading">If WiFi connects on a different channel</div>
    <q-option-group
      class="policy-group"
      :model-value="policy"
      :options="policyOptions"
      type="radio"
      dense
      @update:model-value="setPolicy"
    />

    <q-separator dark class="q-mt-md" />

    <div class="row items-center no-wrap">
      <div class="col-4 text-caption">State</div>
      <div class="col">
        <q-badge v-if="state === 'up'"               color="green">up</q-badge>
        <q-badge v-else-if="state === 'starting'"     color="orange">starting</q-badge>
        <q-badge v-else-if="state === 'channel_conflict'" color="red">channel conflict</q-badge>
        <q-badge v-else-if="state === 'waiting_wifi'" color="orange">waiting wifi</q-badge>
        <q-badge v-else-if="state === 'error'"        color="red">error</q-badge>
        <q-badge v-else                               color="grey">{{ state || 'down' }}</q-badge>
      </div>
    </div>

    <div v-if="state === 'up'" class="row items-center no-wrap">
      <div class="col-4 text-caption">Channel</div>
      <div class="col text-caption">{{ chanEff }}</div>
    </div>
    <div v-if="state === 'up'" class="row items-center no-wrap">
      <div class="col-4 text-caption">Rate</div>
      <div class="col text-caption">{{ rateEff / 1000 }} kbps (long range)</div>
    </div>
    <div v-if="wifiConnected" class="row items-center no-wrap">
      <div class="col-4 text-caption">WiFi</div>
      <div class="col text-caption">{{ wifiSsid }} · ch {{ wifiChannel }}</div>
    </div>
    <div class="row items-center no-wrap">
      <div class="col-4 text-caption">Frames</div>
      <div class="col text-caption">
        rx {{ rxFrames }} (drop {{ rxDrop }}) · tx {{ txFrames }} (fail {{ txFail }})
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from 'diptych-browser/stores/device'

const device = useDeviceStore()

const state    = computed(() => String(device.get('espnow.state') ?? ''))
const channel  = computed(() => Number(device.get('s.espnow.channel') ?? 1))
const conflictCh = computed(() => Number(device.get('espnow.conflict_ch') ?? 0))
const policy   = computed(() => String(device.get('s.espnow.conflict_policy') ?? 'disable'))
function setPolicy(v: string) { device.set('s.espnow.conflict_policy', v) }

const policyOptions = [
  { label: 'Disable ESPnow until WiFi disconnects', value: 'disable' },
  { label: 'Stay on the ESPnow channel — don’t join WiFi networks on other channels, and leave if they change channel', value: 'stay' },
]
const wifiConnected = computed(() => String(device.get('wifi.sta.state') ?? '') === 'connected')
const wifiSsid      = computed(() => String(device.get('wifi.sta.ssid') ?? ''))
const wifiChannel   = computed(() => Number(device.get('wifi.sta.channel') ?? 0))
const chanEff  = computed(() => Number(device.get('espnow.channel_eff') ?? 0))
const rateEff  = computed(() => Number(device.get('espnow.rate_eff') ?? 0))
const rxFrames = computed(() => Number(device.get('espnow.stats.rx_frames') ?? 0))
const txFrames = computed(() => Number(device.get('espnow.stats.tx_frames') ?? 0))
const rxDrop   = computed(() => Number(device.get('espnow.stats.rx_drop') ?? 0))
const txFail   = computed(() => Number(device.get('espnow.stats.tx_fail') ?? 0))

/* Channels the ESP32-S3 2.4 GHz radio drives for ESP-NOW. 1–11 are
 * worldwide; 12 & 13 are not permitted in the US. Channel 14 is
 * 802.11b-only (Japan) and unusable for the long-range PHY, so it is
 * not offered. Stored as strings (SettingSelect convention; the device
 * side atoi's them). */
const chanOptions = [
  { label: '1',  value: '1' },
  { label: '2',  value: '2' },
  { label: '3',  value: '3' },
  { label: '4',  value: '4' },
  { label: '5',  value: '5' },
  { label: '6',  value: '6' },
  { label: '7',  value: '7' },
  { label: '8',  value: '8' },
  { label: '9',  value: '9' },
  { label: '10', value: '10' },
  { label: '11', value: '11' },
  { label: '12 (not US)', value: '12' },
  { label: '13 (not US)', value: '13' },
]
const rateOptions = [
  { label: '250 kbps (long range)', value: '250k' },
  { label: '500 kbps (long range)', value: '500k' },
]
</script>

<style scoped>
.section-heading {
  opacity: 0.6;
  font-size: 13.75px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  margin-top: 22px;
  margin-bottom: -4px;
}
.policy-group :deep(.q-radio:not(:first-child)) {
  margin-top: 5px;
}
</style>
