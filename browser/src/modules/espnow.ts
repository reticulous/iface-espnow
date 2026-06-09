import { useMenuStore } from 'spangap-browser/stores/menu'
import EspnowPanel from '../panels/EspnowPanel.vue'

export function registerEspnow() {
  const menu = useMenuStore()
  menu.setMenu('settings/mesh/interfaces', { label: 'RNS Interfaces', placement: 2 })
  menu.register('settings/mesh/interfaces/espnow', 'ESPnow', { type: 'panel', component: EspnowPanel })
}
