import { useMenuStore } from 'spangap-browser/stores/menu'
import EspnowPanel from '../panels/EspnowPanel.vue'

export function registerEspnow() {
  useMenuStore().register('settings/reticulum/transports/espnow', 'ESPnow', { type: 'panel', component: EspnowPanel })
}
