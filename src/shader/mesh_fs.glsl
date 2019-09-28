// ------------------------------------------------------------------
// OUTPUT VARIABLES  ------------------------------------------------
// ------------------------------------------------------------------

out vec3 FS_OUT_Color;

// ------------------------------------------------------------------
// INPUT VARIABLES  -------------------------------------------------
// ------------------------------------------------------------------

in vec3 FS_IN_WorldPos;
in vec3 FS_IN_Normal;
in vec2 FS_IN_TexCoord;

// ------------------------------------------------------------------
// UNIFORMS ---------------------------------------------------------
// ------------------------------------------------------------------

uniform sampler2D s_Texture;

// ------------------------------------------------------------------
// MAIN -------------------------------------------------------------
// ------------------------------------------------------------------

void main()
{
    vec3 light_pos = vec3(200.0, 200.0, 200.0);
	vec3 n = normalize(FS_IN_Normal);
	vec3 l = normalize(light_pos - FS_IN_WorldPos);
	float lambert = max(0.0f, dot(n, l));
    vec3 diffuse = texture(s_Texture, FS_IN_TexCoord).xyz;
	vec3 ambient = diffuse * 0.03;
	vec3 color = diffuse * lambert + ambient;
    FS_OUT_Color = color;
}

// ------------------------------------------------------------------