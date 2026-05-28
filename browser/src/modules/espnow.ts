import { useMenuStore } from 'spangap-browser/stores/menu'
import EspnowPanel from '../panels/EspnowPanel.vue'

export function registerEspnow() {
  useMenuStore().register('settings', 'Settings', 10, [
    {
      id: 'reticulum', label: 'Reticulum', type: 'submenu', order: 30,
      children: [
        {
          id: 'reticulum.transports', label: 'Transports', type: 'submenu', order: 20,
          children: [
            { id: 'reticulum.transports.espnow', label: 'ESPnow', type: 'panel', order: 20,
              component: EspnowPanel },
          ],
        },
      ],
    },
  ])
}
