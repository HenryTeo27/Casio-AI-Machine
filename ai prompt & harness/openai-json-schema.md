{
  "name": "oled_layout_response",
  "strict": true,
  "schema": {
    "type": "object",
    "additionalProperties": false,
    "required": [
      "oled_layout"
    ],
    "properties": {
      "oled_layout": {
        "type": "array",
        "description": "OLED display layout blocks converted from the complete answer.",
        "items": {
          "type": "object",
          "additionalProperties": false,
          "required": [
            "type",
            "height",
            "lines",
            "latex"
          ],
          "properties": {
            "type": {
              "type": "string",
              "description": "Block type. Use text for normal text blocks, formula for LaTeX formula blocks.",
              "enum": [
                "text",
                "formula"
              ]
            },
            "height": {
              "type": "number",
              "description": "Block rendering height. Text blocks must use 32. Formula blocks use 16, 32, or 64.",
              "enum": [
                16,
                32,
                64
              ]
            },
            "lines": {
              "type": [
                "array",
                "null"
              ],
              "description": "For text blocks only: 1 to 2 display lines. Formula blocks must set this to null.",
              "items": {
                "type": "string"
              }
            },
            "latex": {
              "type": [
                "string",
                "null"
              ],
              "description": "For formula blocks only: valid LaTeX string. Text blocks must set this to null."
            }
          }
        }
      }
    }
  }
}