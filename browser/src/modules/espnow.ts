import { useMenuStore } from 'spangap-browser/stores/menu'
import EspnowPanel from '../panels/EspnowPanel.vue'

export function registerEspnow() {
  useMenuStore().register('settings', 'Settings', [
    {
      id: 'reticulum', label: 'Reticulum', type: 'submenu',
      children: [
        {
          id: 'reticulum.transports', label: 'Transports', type: 'submenu',
          children: [
            { id: 'reticulum.transports.espnow', label: 'ESPnow', type: 'panel',
              component: EspnowPanel },
          ],
        },
      ],
    },
  ])
}
