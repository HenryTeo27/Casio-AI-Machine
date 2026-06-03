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
              "description": "Block type. Use text for normal text blocks, formula for LaTeX formula blocks, and diagram for ASCII circuit/PLC/control diagrams.",
              "enum": [
                "text",
                "formula",
                "diagram"
              ]
            },
            "height": {
              "type": "integer",
              "description": "Block rendering height in pixels. Text blocks must use 32. Formula blocks use 16, 32, or 64. Diagram blocks use lines.length × 16 and may be larger than 64."
            },
            "lines": {
              "type": [
                "array",
                "null"
              ],
              "description": "For text blocks: 1 to 2 display lines. For diagram blocks: all original ASCII diagram lines. Formula blocks must set this to null.",
              "items": {
                "type": "string"
              }
            },
            "latex": {
              "type": [
                "string",
                "null"
              ],
              "description": "For formula blocks only: valid LaTeX string. Text and diagram blocks must set this to null."
            }
          }
        }
      }
    }
  }
}