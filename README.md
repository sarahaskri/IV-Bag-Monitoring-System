# IV Bag Monitoring System

Prototype IoT de surveillance d'une poche de perfusion avec ESP32. Le firmware suit le poids de la poche, estime le débit et le temps restant, repère une absence prolongée de variation et déclenche une alerte locale.

> **Prototype académique uniquement.** Ce projet n'est pas un dispositif médical certifié et ne doit pas servir à prendre des décisions de soin.

## Présentation

Le système associe un ESP32 à un capteur de poids HX711, un capteur analogique d'eau/niveau, un écran LCD, une LED et un buzzer. L'ESP32 envoie les mesures en HTTP POST à un backend Node.js. L'application Flutter peut consulter les données en HTTP GET, conformément à l'architecture décrite dans la présentation.

## Architecture

~~~mermaid
flowchart LR
    Bag[Poche de perfusion] --> Load[Cellule de charge + HX711]
    Bag --> Water[Capteur d'eau / niveau]
    Load --> ESP[ESP32]
    Water --> ESP
    ESP --> LCD[LCD 16x2]
    ESP --> Alarm[LED + buzzer]
    ESP -->|HTTP POST| API[Backend Node.js]
    API --> DB[(MongoDB)]
    App[Application Flutter] -->|HTTP GET| API
~~~

## Fonctions du firmware

- Lecture du poids de la poche.
- Estimation du débit à partir de la baisse de poids, avec l'approximation **1 g ≈ 1 mL**.
- Estimation du contenu restant et du temps restant quand un débit est mesuré.
- Alerte locale lorsque le contenu passe sous le seuil configuré.
- Alerte lorsque le capteur d'eau dépasse son seuil.
- Détection d'une obstruction **suspectée** lorsque le poids reste stable au-delà de la durée réglée.
- Envoi périodique des mesures et envoi lors d'un changement d'état.

La détection d'obstruction est indirecte : elle repose sur l'absence de baisse de poids. Le système ne mesure pas directement la pression ou le débit dans le tube.

## Matériel et câblage

| Élément | Broche ESP32 / réglage |
| --- | --- |
| HX711 DOUT | GPIO 4 |
| HX711 SCK | GPIO 5 |
| Capteur analogique d'eau/niveau | GPIO 34 |
| LED | GPIO 18 |
| Buzzer actif | GPIO 19 |
| LCD I2C SDA | GPIO 21 |
| LCD I2C SCL | GPIO 22 |
| Adresse LCD | 0x27 par défaut |

Vérifie le câblage réel avant l'alimentation. Les broches d'entrée de l'ESP32 ne tolèrent pas 5 V.

## Démarrage

1. Ouvre le sketch dans Arduino IDE : firmware/esp32_iv_bag_monitor/esp32_iv_bag_monitor.ino.
2. Sélectionne une carte ESP32 compatible et installe les bibliothèques **HX711** et **LiquidCrystal_I2C**.
3. Renseigne WIFI_SSID, WIFI_PASSWORD et BACKEND_URL dans le sketch.
4. Calibre la balance et le capteur d'eau, puis ajuste le poids à vide et les seuils.
5. Téléverse le sketch et observe les mesures à 115200 bauds dans le moniteur série.

Le sketch ne contient aucun vrai identifiant Wi-Fi. Garde les identifiants dans une copie locale non suivie par Git.

## Calibration requise

Les valeurs du brouillon sont reprises comme valeurs de départ : facteur HX711 **-400**, seuil bas **50 g** et seuil capteur **100**. Elles doivent être réglées sur le montage réel. EMPTY_BAG_TARE_G vaut **0** par défaut ; renseigne le poids du sac vide et de son support pour mesurer le contenu net.

L'estimation **1 g ≈ 1 mL** est une approximation. Un seuil de variation trop élevé peut masquer un débit lent ; un seuil trop bas peut réagir au bruit de la cellule de charge.

## API backend

Le sketch envoie un objet JSON de cette forme à l'URL configurée :

~~~json
{
  "deviceId": "iv-monitor-01",
  "weightG": 420.0,
  "remainingContentG": 420.0,
  "waterSensorRaw": 0,
  "flowMlMin": 12.4,
  "remainingMinutes": 33.9,
  "status": "normal"
}
~~~

Si aucun débit n'est mesuré, remainingMinutes vaut null. L'URL, la route et les noms de champs sont des exemples à adapter à l'API Node.js réelle.

## Ressources

- [Présentation du projet (PDF)](IV%20Bag%20Monitoring%20System.pdf)
- [Vidéo de démonstration](Demo.mp4)

## Limites connues

- Le facteur HX711 et les seuils doivent être calibrés.
- Le capteur d'eau doit être caractérisé pour déterminer si son seuil signifie présence d'eau, niveau bas ou fuite.
- Le débit et le temps restant sont des estimations expérimentales.
- Le POST dépend du schéma et de la route du serveur, qui doivent être raccordés.
- Le servo-moteur mentionné comme perspective n'est pas implémenté dans ce firmware.

